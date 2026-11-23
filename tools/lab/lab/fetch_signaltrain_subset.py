"""Stream a small (~50 MB) paired input/target subset from Zenodo 3824876 (SignalTrain LA-2A)
without downloading the full 20.7 GB archive.
"""

from pathlib import Path
import tarfile
import urllib.request

URL = "https://zenodo.org/api/records/3824876/files/SignalTrain_LA2A_Dataset_1.1.tgz/content"
OUT_DIR = Path("../../data/signaltrain_subset")
OUT_DIR.mkdir(parents=True, exist_ok=True)

MAX_BYTES = 55 * 1024 * 1024  # 55 MB cap
extracted_bytes = 0
saved_files = []

req = urllib.request.Request(URL, headers={"User-Agent": "StoneridgeDSP-Lab/0.1"})
with urllib.request.urlopen(req, timeout=30) as resp:
    with tarfile.open(fileobj=resp, mode="r|gz") as tf:
        for member in tf:
            if not member.isfile():
                continue
            name = member.name
            # Extract metadata/README or a clean spread of input/target WAV pairs
            if name.endswith((".txt", ".json", ".csv", ".md")) or (
                name.endswith(".wav") and ("Train" in name or "Val" in name or "Test" in name)
            ):
                if extracted_bytes + member.size > MAX_BYTES and len(saved_files) >= 12:
                    break
                rel = Path(name)
                # Flatten top-level archive folder if present
                parts = rel.parts[1:] if len(rel.parts) > 1 else rel.parts
                dest = OUT_DIR.joinpath(*parts)
                dest.parent.mkdir(parents=True, exist_ok=True)
                f_in = tf.extractfile(member)
                if f_in is not None:
                    data = f_in.read()
                    dest.write_bytes(data)
                    extracted_bytes += len(data)
                    saved_files.append((str(Path(*parts)), len(data)))
                    print(f"Extracted {Path(*parts)} ({len(data)/1e6:.2f} MB) | total={extracted_bytes/1e6:.1f} MB")
            if extracted_bytes >= MAX_BYTES and len(saved_files) >= 12:
                break

print(f"Done: {len(saved_files)} files, {extracted_bytes/1e6:.2f} MB total")
