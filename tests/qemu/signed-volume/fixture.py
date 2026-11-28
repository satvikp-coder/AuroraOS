"""Generate an ephemeral public test root and four-sector signed AURMODEL fixture."""
import hashlib
from pathlib import Path
import struct
import subprocess
import sys


def openssl(*args, data=None):
    return subprocess.run(["openssl", *args], input=data, stdout=subprocess.PIPE,
                          check=True).stdout


def emit(name, value):
    return f"static const uint8_t {name}[{len(value)}] = {{" + ",".join(map(str, value)) + "};\n"


def generate(output_dir):
    secret = openssl("genpkey", "-algorithm", "ED25519")
    public_der = openssl("pkey", "-pubout", "-outform", "DER", data=secret)
    prefix = bytes.fromhex("302a300506032b6570032100")
    if len(public_der) != len(prefix) + 32 or not public_der.startswith(prefix):
        raise ValueError("unexpected Ed25519 SubjectPublicKeyInfo")
    public = public_der[len(prefix):]
    payload = b"AuroraOS signed volume probe"
    header, table, trailer = (bytearray(512) for _ in range(3))
    table[:7] = b"example"
    struct.pack_into("<QQQ", table, 32, len(payload), 2, 1)
    table[64:96] = hashlib.sha256(payload).digest()
    struct.pack_into("<8sIIQIII", header, 0, b"AURMODEL", 2, 512, 4, 1, 1, 1)
    header[64:96] = hashlib.sha256(table[:128]).digest()
    header[96:128] = hashlib.sha256(header[:96]).digest()
    struct.pack_into("<4sIII", trailer, 0, b"AURS", 2, 3, 7)
    trailer[16:48] = header[96:128]
    # Pure Ed25519 needs a seekable message for the OpenSSL one-shot interface.
    # Only public metadata goes to disk. The ephemeral key travels via stdin.
    message = output_dir / "signed-prefix.bin"
    message.write_bytes(trailer[:48])
    signature = openssl("pkeyutl", "-sign", "-rawin", "-inkey", "/dev/stdin",
                        "-in", str(message), data=secret)
    if len(signature) != 64:
        raise ValueError("unexpected signature length")
    trailer[48:112] = signature
    image = header + table + payload.ljust(512, b"\0") + trailer
    (output_dir / "signed_fixture.h").write_text(emit("public_key", public) +
                                                emit("signed_image", image), encoding="ascii")


if __name__ == "__main__":
    generate(Path(sys.argv[1]))
