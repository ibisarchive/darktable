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
                            output 1x768 embedding (not normalised); static shapes
    <out>/text_embeds.bin   N x 768 float32, L2-normalised, row i = labels line i
    <out>/labels.txt        eBird common name per row
    <out>/species.csv       species_code,scientific_name,common_name,taxon_order,family

Usage:
    python export_bioclip.py --taxonomy ebird_taxonomy.csv --out <dir> [--fp16] [--skip-text]

Run once by the developer; the result is shipped/downloaded, not built on
the user's machine. --skip-text reuses the text embeddings already in
--out (the slow part) and only re-exports the image tower.
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

MEAN = [0.48145466, 0.4578275, 0.40821073]
STD = [0.26862954, 0.26130258, 0.27577711]


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


def embed_text(model, tokenizer, species, batch):
    """Mean of the prompt embeddings per species, L2-normalised."""
    embeds = []
    with torch.no_grad():
        for i in range(0, len(species), batch):
            chunk = species[i:i + batch]
            per_prompt = []
            for tpl in PROMPTS:
                texts = [tpl.format(common=s["common"], sci=s["sci"]) for s in chunk]
                t = model.encode_text(tokenizer(texts))
                t = t / t.norm(dim=-1, keepdim=True)
                per_prompt.append(t)
            e = torch.stack(per_prompt).mean(0)
            e = e / e.norm(dim=-1, keepdim=True)
            embeds.append(e.float().cpu().numpy())
            print(f"  text {min(i + batch, len(species))}/{len(species)}", end="\r", flush=True)
    embeds = np.concatenate(embeds).astype(np.float32)
    print(f"\ntext embeddings {embeds.shape}")
    return embeds


def write_tables(out, species, embeds):
    embeds.tofile(os.path.join(out, "text_embeds.bin"))
    with open(os.path.join(out, "labels.txt"), "w", encoding="utf-8") as f:
        for s in species:
            f.write(s["common"] + "\n")
    with open(os.path.join(out, "species.csv"), "w", encoding="utf-8", newline="") as f:
        w = csv.writer(f)
        w.writerow(["species_code", "scientific_name", "common_name", "taxon_order", "family"])
        for s in species:
            w.writerow([s["code"], s["sci"], s["common"], s["order"], s["family"]])


class Visual(torch.nn.Module):
    def __init__(self, m):
        super().__init__()
        self.m = m

    def forward(self, x):
        return self.m.encode_image(x)


def export_tower(model, onnx_path, fp16):
    """Image tower to ONNX with a fixed batch of one: darktable's backend
    feeds one frame at a time, and static shapes keep it on the plain
    caller-allocated output path."""
    vis = Visual(model).eval()
    dummy = torch.zeros(1, 3, 224, 224)
    torch.onnx.export(
        vis, dummy, onnx_path,
        input_names=["image"], output_names=["embedding"],
        opset_version=17, dynamo=False,
    )
    if fp16:
        import onnx
        from onnxruntime.transformers.float16 import convert_float_to_float16
        m = onnx.load(onnx_path)
        m = convert_float_to_float16(m, keep_io_types=True)
        onnx.save(m, onnx_path)
    print(f"image tower -> {onnx_path} ({os.path.getsize(onnx_path) / 1e6:.0f} MB)")

    import onnxruntime as ort
    sess = ort.InferenceSession(onnx_path, providers=["CPUExecutionProvider"])
    print("onnx input", [(i.name, i.shape) for i in sess.get_inputs()],
          "output", [(o.name, o.shape) for o in sess.get_outputs()])
    with torch.no_grad():
        ref = vis(dummy).numpy()
    got = sess.run(None, {"image": dummy.numpy()})[0]
    print(f"onnx vs torch max abs diff {np.abs(ref - got).max():.4g}")


def write_manifest(out, model_name, n_species, embed_dim, logit_scale):
    manifest = {
        "id": "classify-birds-global",
        "name": f"bird species classifier (eBird taxonomy, {n_species} species, {model_name})",
        "description": "BioCLIP 2 image tower with text embeddings of every eBird species. "
                       "Input 'image' float32 1x3x224x224, RGB, shortest side resized to 224 (bicubic), "
                       "center crop, normalised with mean/std below. Output 'embedding' 1x768; "
                       "cosine against text_embeds.bin rows, times logit_scale, softmax.",
        "task": "classify",
        "backend": "onnx",
        "version": "1.1",
        "num_inputs": 1,
        "arch": "bioclip",
        "ort_optimization": "basic",
        "attributes": {
            "input_size": 224,
            "mean": MEAN,
            "std": STD,
            "embeddings": "text_embeds.bin",
            "embed_dim": int(embed_dim),
            "logit_scale": logit_scale,
            "labels": "labels.txt",
            "species": "species.csv",
            "source": f"imageomics/{model_name} (MIT)",
        },
    }
    with open(os.path.join(out, "config.json"), "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--taxonomy", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--model", default="bioclip-2", choices=["bioclip-2", "bioclip"])
    ap.add_argument("--fp16", action="store_true", help="store the image tower in float16")
    ap.add_argument("--batch", type=int, default=256)
    ap.add_argument("--skip-text", action="store_true",
                    help="reuse text_embeds.bin/labels.txt/species.csv already in --out")
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    species = load_species(args.taxonomy)
    print(f"{len(species)} species from {args.taxonomy}")

    hf = f"hf-hub:imageomics/{args.model}"
    model, _, _ = open_clip.create_model_and_transforms(hf)
    tokenizer = open_clip.get_tokenizer(hf)
    model.eval()
    logit_scale = float(model.logit_scale.exp().item())
    print(f"model {hf}, logit scale {logit_scale:.2f}")

    embeds_path = os.path.join(args.out, "text_embeds.bin")
    if args.skip_text and os.path.exists(embeds_path):
        embeds = np.fromfile(embeds_path, dtype=np.float32).reshape(len(species), -1)
        print(f"reusing text embeddings {embeds.shape}")
    else:
        embeds = embed_text(model, tokenizer, species, args.batch)
        write_tables(args.out, species, embeds)

    export_tower(model, os.path.join(args.out, "model.onnx"), args.fp16)
    write_manifest(args.out, args.model, len(species), embeds.shape[1], logit_scale)
    print("done:", args.out)


if __name__ == "__main__":
    sys.exit(main())
