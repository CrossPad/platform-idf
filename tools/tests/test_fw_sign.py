import subprocess, sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import fw_sign

REPO = Path(__file__).resolve().parents[2]


def keypair(tmp_path, name="k", curve="prime256v1"):
    key = tmp_path / f"{name}.key.pem"
    pub = tmp_path / f"{name}.pub.pem"
    subprocess.run(["openssl", "ecparam", "-name", curve, "-genkey", "-noout", "-out", str(key)], check=True)
    subprocess.run(["openssl", "ec", "-in", str(key), "-pubout", "-out", str(pub)], check=True,
                   capture_output=True)
    return key, pub


def manifest(tmp_path, text='{"kit": "full"}\n'):
    p = tmp_path / "manifest.json"
    p.write_text(text)
    return p


def test_sign_round_trip(tmp_path):
    key, pub = keypair(tmp_path)
    m = manifest(tmp_path)
    sig = fw_sign.sign(m, key)
    assert sig[0] == 0x30 and len(sig) <= 72            # DER SEQUENCE, P-256 size
    assert fw_sign.verify(m, sig, pub)


def test_a_flipped_manifest_byte_fails(tmp_path):
    key, pub = keypair(tmp_path)
    m = manifest(tmp_path)
    sig = fw_sign.sign(m, key)
    data = bytearray(m.read_bytes())
    data[3] ^= 0x01
    m.write_bytes(bytes(data))
    assert not fw_sign.verify(m, sig, pub)


def test_another_key_fails(tmp_path):
    key, _ = keypair(tmp_path, "a")
    _, other = keypair(tmp_path, "b")
    m = manifest(tmp_path)
    assert not fw_sign.verify(m, fw_sign.sign(m, key), other)


def test_refuses_a_key_that_is_not_p256(tmp_path):
    key, _ = keypair(tmp_path, "big", curve="secp384r1")
    with pytest.raises(SystemExit, match="P-256"):
        fw_sign.sign(manifest(tmp_path), key)


def test_header_round_trip_and_verify(tmp_path):
    key, pub = keypair(tmp_path)
    der = fw_sign.public_der(pub)
    text = fw_sign.header_text([der])
    assert "#define FW_SIGN_KEY_COUNT 1" in text
    assert fw_sign.header_keys(text) == [der]
    header = tmp_path / "fw_sign_pubkey.h"
    header.write_text(text)
    m = manifest(tmp_path)
    assert fw_sign.verify_with_header(header, m, fw_sign.sign(m, key))
    other_key, _ = keypair(tmp_path, "other")
    assert not fw_sign.verify_with_header(header, m, fw_sign.sign(m, other_key))


def test_header_without_keys_and_with_too_many(tmp_path):
    text = fw_sign.header_text([])
    assert "#define FW_SIGN_KEY_COUNT 0" in text
    assert text.count("{nullptr, 0},") == 2
    assert fw_sign.header_keys(text) == []
    ders = [fw_sign.public_der(keypair(tmp_path, f"k{i}")[1]) for i in range(3)]
    with pytest.raises(SystemExit, match="at most 2"):
        fw_sign.header_text(ders)


def test_committed_header_matches_the_public_keys_in_config():
    # A header generated from any other key (a local test key, say) must
    # never reach a commit: release images would then trust that key.
    pems = sorted((REPO / "config" / "fw_signing").glob("*.pub.pem"))
    expected = fw_sign.header_text([fw_sign.public_der(p) for p in pems])
    assert (REPO / "main" / "fw_sign_pubkey.h").read_text() == expected
