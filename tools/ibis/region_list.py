#!/usr/bin/env python3
"""Write the eBird species list of a region as one common name per line.

identify birds reads such a file (conf plugins/lighttable/ibis_identify/
region_list) and only considers those species: a Norwegian river cannot
hold a Limpkin, so the classifier should not be allowed to say one.

    python region_list.py --region NO-11 --out regions/NO-11.txt
    python region_list.py --lat 58.65 --lon 6.17 --country NO --out regions/auto.txt

With --lat/--lon the region is the subnational1 code of the nearest eBird
hotspot in --country (eBird has no reverse geocoder; hotspots carry their
region). Needs an eBird API key (--key, EBIRD_API_KEY, or the Kestrel
settings.json) and the taxonomy CSV to turn species codes into names.
"""
import argparse
import csv
import json
import math
import os
import sys
import urllib.request

API = "https://api.ebird.org/v2"


def get(url, key):
    req = urllib.request.Request(url, headers={"x-ebirdapitoken": key})
    with urllib.request.urlopen(req, timeout=60) as r:
        return r.read().decode("utf-8")


def find_key(explicit):
    if explicit:
        return explicit
    if os.environ.get("EBIRD_API_KEY"):
        return os.environ["EBIRD_API_KEY"]
    p = os.path.join(os.environ.get("LOCALAPPDATA", ""), "ProjectKestrel", "settings.json")
    if os.path.exists(p):
        with open(p, encoding="utf-8") as f:
            return json.load(f).get("ebird_api_key", "")
    return ""


def nearest_region(lat, lon, country, key, cache_dir):
    path = os.path.join(cache_dir, f"hotspots_{country}.json")
    if os.path.exists(path):
        with open(path, encoding="utf-8") as f:
            hot = json.load(f)
    else:
        hot = json.loads(get(f"{API}/ref/hotspot/{country}?fmt=json", key))
        os.makedirs(cache_dir, exist_ok=True)
        with open(path, "w", encoding="utf-8") as f:
            json.dump(hot, f)
    best, best_d = None, 1e12
    for h in hot:
        d = math.hypot((h["lat"] - lat) * 111.0, (h["lng"] - lon) * 111.0 * math.cos(math.radians(lat)))
        if d < best_d:
            best, best_d = h, d
    if not best:
        raise SystemExit(f"no hotspots for {country}")
    print(f"nearest hotspot: {best['locName']} ({best_d:.1f} km) -> {best['subnational1Code']}")
    return best["subnational1Code"]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--region", help="eBird region code, e.g. NO-11 or NO")
    ap.add_argument("--lat", type=float)
    ap.add_argument("--lon", type=float)
    ap.add_argument("--country", default="NO")
    ap.add_argument("--key")
    ap.add_argument("--taxonomy", default=os.path.join(os.environ.get("LOCALAPPDATA", ""), "ProjectKestrel", "ebird", "ebird_taxonomy.csv"))
    ap.add_argument("--cache", default=os.path.join(os.environ.get("LOCALAPPDATA", ""), "ProjectKestrel", "ebird"))
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    key = find_key(args.key)
    if not key:
        raise SystemExit("no eBird API key")
    region = args.region
    if not region:
        if args.lat is None or args.lon is None:
            raise SystemExit("give --region or --lat/--lon")
        region = nearest_region(args.lat, args.lon, args.country, key, args.cache)

    codes = json.loads(get(f"{API}/product/spplist/{region}", key))
    names = {}
    with open(args.taxonomy, encoding="utf-8-sig", newline="") as f:
        for r in csv.DictReader(f):
            if r["CATEGORY"] == "species":
                names[r["SPECIES_CODE"]] = r["COMMON_NAME"]
    out = [names[c] for c in codes if c in names]
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as f:
        f.write(f"# eBird species list for {region}, {len(out)} species\n")
        for n in out:
            f.write(n + "\n")
    print(f"{region}: {len(out)} species -> {args.out}")


if __name__ == "__main__":
    sys.exit(main())
