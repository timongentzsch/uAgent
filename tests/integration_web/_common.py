import base64
import subprocess

PNG = base64.b64decode(
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+a5WQAAAAASUVORK5CYII="
)
ASSET_NAME_CHARS = 128
PARTIAL_USAGE_GATE_SECONDS = 10
PARTIAL_USAGE_REQUEST_SECONDS = 25


def p256_public_key():
    """A fresh uncompressed P-256 point, as a browser's p256dh key."""
    pem = subprocess.run(
        ["openssl", "ecparam", "-name", "prime256v1", "-genkey", "-noout"],
        capture_output=True,
        check=True,
    ).stdout
    der = subprocess.run(
        ["openssl", "ec", "-pubout", "-outform", "DER"],
        input=pem,
        capture_output=True,
        check=True,
    ).stdout
    return base64.urlsafe_b64encode(der[-65:]).rstrip(b"=").decode()
