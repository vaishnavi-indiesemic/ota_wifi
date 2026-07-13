import argparse
import hashlib
import json
import os
import zlib

def calculate_sha256(filepath):
    sha256_hash = hashlib.sha256()
    with open(filepath, "rb") as f:
        # Read file in blocks to avoid memory issues with large files
        for byte_block in iter(lambda: f.read(4096), b""):
            sha256_hash.update(byte_block)
    return sha256_hash.hexdigest()

def calculate_crc32(filepath):
    with open(filepath, "rb") as f:
        # zlib.crc32 calculates the CRC32 of the bytes
        crc32_val = zlib.crc32(f.read())
    # Format as exactly 8 lowercase hexadecimal characters
    return f"{crc32_val & 0xFFFFFFFF:08x}"

def main():
    parser = argparse.ArgumentParser(description="Generate update.json for Nordic OTA")
    parser.add_argument("--bin", required=True, help="Path to the zephyr.signed.bin file")
    parser.add_argument("--version", required=True, help="Firmware version (e.g., 1.1.0)")
    parser.add_argument("--fw-path", required=True, help="GitHub path to the firmware (e.g., /BareMetalBits/OTA_Nordic_Server/main/zephyr.signed.bin)")
    parser.add_argument("--output", default="update.json", help="Output JSON file name (default: update.json)")
    
    args = parser.parse_args()

    if not os.path.exists(args.bin):
        print(f"Error: Binary file '{args.bin}' not found.")
        return

    # Calculate values
    file_size = os.path.getsize(args.bin)
    sha256 = calculate_sha256(args.bin)
    crc32 = calculate_crc32(args.bin)

    # Build the manifest dictionary
    manifest = {
        "version": args.version,
        "fw_path": args.fw_path,
        "file_size": file_size,
        "sha256": sha256,
        "crc32": crc32
    }

    # Write to update.json
    with open(args.output, "w") as f:
        json.dump(manifest, f, indent=2)

    print(f"✅ Successfully generated {args.output}!")
    print("--------------------------------------------------")
    print(json.dumps(manifest, indent=2))
    print("--------------------------------------------------")

if __name__ == "__main__":
    main()
