#!/bin/bash

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
SHADOW_DIR="$ROOT_DIR/shadow"

mkdir -p "$SHADOW_DIR"

# Passwords spread across the search space (79^3 = 493,039 candidates)
PASSWORD_MD5="ABC"
PASSWORD_BCRYPT="DEF"
PASSWORD_SHA256="GHI"
PASSWORD_SHA512="JKL"
PASSWORD_YESCRYPT="MNO"

SALT="saltsalt"
BCRYPT_SALT='$2b$05$saltsaltsaltsaltsalt12'
YESCRYPT_SALT='$y$j9T$n34PoBLMgFrQVl4Rn34Po/'

echo "Generating shadow file entries..."
echo ""

echo "=== MD5 ==="
HASH_MD5=$("$ROOT_DIR/gen_hash" "$PASSWORD_MD5" "\$1\$$SALT\$")
echo "Password: $PASSWORD_MD5"
echo "Hash: $HASH_MD5"
echo "testuser:$HASH_MD5:18900:0:99999:7:::" > "$SHADOW_DIR/shadow_test_md5.txt"
echo ""

echo "=== bcrypt ==="
HASH_BCRYPT=$("$ROOT_DIR/gen_hash" "$PASSWORD_BCRYPT" "$BCRYPT_SALT")
if [ $? -eq 0 ] && [ -n "$HASH_BCRYPT" ]; then
    echo "Password: $PASSWORD_BCRYPT"
    echo "Hash: $HASH_BCRYPT"
    echo "testuser:$HASH_BCRYPT:18900:0:99999:7:::" > "$SHADOW_DIR/shadow_test_bcrypt.txt"
else
    echo "Error: Failed to generate bcrypt hash"
    exit 1
fi
echo ""

echo "=== SHA-256 ==="
HASH_SHA256=$("$ROOT_DIR/gen_hash" "$PASSWORD_SHA256" "\$5\$$SALT\$")
echo "Password: $PASSWORD_SHA256"
echo "Hash: $HASH_SHA256"
echo "testuser:$HASH_SHA256:18900:0:99999:7:::" > "$SHADOW_DIR/shadow_test_sha256.txt"
echo ""

echo "=== SHA-512 ==="
HASH_SHA512=$("$ROOT_DIR/gen_hash" "$PASSWORD_SHA512" "\$6\$$SALT\$")
echo "Password: $PASSWORD_SHA512"
echo "Hash: $HASH_SHA512"
echo "testuser:$HASH_SHA512:18900:0:99999:7:::" > "$SHADOW_DIR/shadow_test_sha512.txt"
echo ""

echo "=== yescrypt ==="
HASH_YESCRYPT=$("$ROOT_DIR/gen_hash" "$PASSWORD_YESCRYPT" "$YESCRYPT_SALT")
if [ $? -eq 0 ] && [ -n "$HASH_YESCRYPT" ]; then
    echo "Password: $PASSWORD_YESCRYPT"
    echo "Hash: $HASH_YESCRYPT"
    echo "testuser:$HASH_YESCRYPT:18900:0:99999:7:::" > "$SHADOW_DIR/shadow_test_yescrypt.txt"
else
    echo "Error: Failed to generate yescrypt hash"
    exit 1
fi
echo ""

echo "Done! Generated shadow files:"
ls -l "$SHADOW_DIR"/shadow_test_*.txt

echo ""
echo "Password summary:"
echo "  MD5:      $PASSWORD_MD5"
echo "  bcrypt:   $PASSWORD_BCRYPT"
echo "  SHA-256:  $PASSWORD_SHA256"
echo "  SHA-512:  $PASSWORD_SHA512"
echo "  yescrypt: $PASSWORD_YESCRYPT"

echo ""
echo "Verifying format (each file):"
for f in "$SHADOW_DIR"/shadow_test_*.txt; do
    echo "  $f: $(head -1 "$f")"
done
