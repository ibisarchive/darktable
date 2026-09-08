#!/usr/bin/env python3
"""Build the Ibis Archive global bird classifier from BioCLIP 2.

BioCLIP 2 (imageomics/bioclip-2, MIT) is a CLIP model trained on 200M
organism images. Zero-shot: an image embedding is compared with text
embeddings of species names, so the label set is whatever we embed. Here
that is eBird's taxonomy (every species, ~11,000), which means the
classifier's answers are eBird names by construction.

Outputs a darktable AI model directory:

    <out>/config.json       darktable manifest (task classify, arch bioclip)
    <out>/model.onnx        image tower, input 1x3x224x224 float32 CLIP-normalised,
                            output 1x768 embedding (not normalised)
    <out>/text_embeds.bin   N x 768 float32, L2-normalised, row i = labels line i
    <out>/labels.txt        eBird common name per row
    <out>/species.csv       species_code,scientific_name,common_name,taxon_order,family

Usage:
    python export_bioclip.py --taxonomy ebird_taxonomy.csv --out <dir> [--fp16] [--model bioclip-2]

Run once by the developer; the result is shipped/downloaded, not built on
the user's machine.
"""
import argparse
import csv
import json
import os
import sys

import numpy as np
import torch
import open_clip

PROMPTS = (
    "a photo of {common}.",
    "a photo of {sci}.",
    "a photo of {common} ({sci}), a species of bird.",
)


def load_species(path):
    rows = []
    with open(path, encoding="utf-8-sig", newline="") as f:
        for r in csv.DictReader(f):
            if (r.get("CATEGORY") or "").strip() != "species":
                continue
            rows.append({
                "code": r["SPECIES_CODE"],
                "sci": r["SCIENTIFIC_NAME"].strip(),
                "common": r["COMMON_NAME"].strip(),
                "order": float(r.get("TAXON_ORDER") or 0),
                "family": (r.get("FAMILY_COM_NAME") or "").strip(),
            })
    rows.sort(key=lambda r: r["order"])
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--taxonomy", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--model", default="bioclip-2", choices=["bioclip-2", "bioclip"])
    ap.add_argument("--fp16", action="store_true", help="store the image tower in float16")
    ap.add_argument("--batch", type=int, default=256)
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    species = load_species(args.taxonomy)
    print(f"{len(species)} species from {args.taxonomy}")

    hf = f"hf-hub:imageomics/{args.model}"
    model, _, preprocess = open_clip.create_model_and_transforms(hf)
    tokenizer = open_clip.get_tokenizer(hf)
    model.eval()
    logit_scale = float(model.logit_scale.exp().item())
    print(f"model {hf}, embed dim {model.text_projection.shape[-1] if hasattr(model, 'text_projection') and model.text_projection is not None else '?'}, logit scale {logit_scale:.2f}")

    # --- text embeddings: mean of prompt embeddings per species, L2-normalised
    embeds = []
    with torch.no_grad():
        for i in range(0, len(species), args.batch):
            chunk = species[i:i + args.batch]
            per_prompt = []
            for tpl in PROMPTS:
                texts = [tpl.format(common=s["common"], sci=s["sci"]) for s in chunk]
                t = model.encode_text(tokenizer(texts))
                t = t / t.norm(dim=-1, keepdim=True)
                per_prompt.append(t)
            e = torch.stack(per_prompt).mean(0)
            e = e / e.norm(dim=-1, keepdim=True)
            embeds.append(e.float().cpu().numpy())
            print(f"  text {min(i + args.batch, len(species))}/{len(species)}", end="\r")
    embeds = np.concatenate(embeds).astype(np.float32)
    print(f"\ntext embeddings {embeds.shape}")
    embeds.tofile(os.path.join(args.out, "text_embeds.bin"))

    with open(os.path.join(args.out, "labels.txt"), "w", encoding="utf-8") as f:
        for s in species:
            f.write(s["common"] + "\n")
    with open(os.path.join(args.out, "species.csv"), "w", encoding="utf-8", newline="") as f:
        w = csv.writer(f)
        w.writerow(["species_code", "scientific_name", "common_name", "taxon_order", "family"])
        for s in species:
            w.writerow([s["code"], s["sci"], s["common"], s["order"], s["family"]])

    # --- image tower to ONNX
    class Visual(torch.nn.Module):
        def __init__(self, m):
            super().__init__()
            self.m = m

        def forward(self, x):
            return self.m.encode_image(x)

    vis = Visual(model).eval()
    dummy = torch.zeros(1, 3, 224, 224)
    onnx_path = os.path.join(args.out, "model.onnx")
    torch.onnx.export(
        vis, dummy, onnx_path,
        input_names=["image"], output_names=["embedding"],
        dynamic_axes={"image": {0: "batch"}, "embedding": {0: "batch"}},
        opset_version=17, dynamo=False,
    )
    if args.fp16:
        import onnx
        from onnxruntime.transformers.float16 import convert_float_to_float16
        m = onnx.load(onnx_path)
        m = convert_float_to_float16(m, keep_io_types=True)
        onnx.save(m, onnx_path)
    print(f"image tower -> {onnx_path} ({os.path.getsize(onnx_path) / 1e6:.0f} MB)")

    # --- sanity: the ONNX tower matches torch on the dummy
    import onnxruntime as ort
    sess = ort.InferenceSession(onnx_path, providers=["CPUExecutionProvider"])
    with torch.no_grad():
        ref = vis(dummy).numpy()
    got = sess.run(None, {"image": dummy.numpy()})[0]
    print(f"onnx vs torch max abs diff {np.abs(ref - got).max():.4g}")

    cfg = preprocess.transforms
    mean = [0.48145466, 0.4578275, 0.40821073]
    std = [0.26862954, 0.26130258, 0.27577711]
    for t in cfg:
        if hasattr(t, "mean"):
            mean, std = [float(v) for v in t.mean], [float(v) for v in t.std]
    manifest = {
        "id": "classify-birds-global",
        "name": f"bird species classifier (eBird taxonomy, {len(species)} species, {args.model})",
        "description": "BioCLIP 2 image tower with text embeddings of every eBird species. "
                       "Input 'image' float32 1x3x224x224, RGB, shortest side resized to 224 (bicubic), "
                       "center crop, normalised with mean/std below. Output 'embedding' 1x768; "
                       "cosine against text_embeds.bin rows, times logit_scale, softmax.",
        "task": "classify",
        "backend": "onnx",
        "version": "1.0",
        "num_inputs": 1,
        "arch": "bioclip",
        "attributes": {
            "input_size": 224,
            "mean": mean,
            "std": std,
            "embeddings": "text_embeds.bin",
            "embed_dim": int(embeds.shape[1]),
            "logit_scale": logit_scale,
            "labels": "labels.txt",
            "species": "species.csv",
            "source": f"imageomics/{args.model} (MIT)",
        },
    }
    with open(os.path.join(args.out, "config.json"), "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2)
    print("done:", args.out)


if __name__ == "__main__":
    sys.exit(main())
