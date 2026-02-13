PASSWORD_MD5="!!!"
PASSWORD_BCRYPT="!!!"
PASSWORD_SHA256="!!!"
PASSWORD_SHA512="!!!"
PASSWORD_YESCRYPT="!!!"

echo "Generating shadow file entries..."

echo "=== MD5 ==="
HASH_MD5=$(openssl passwd -1 -salt saltsalt "$PASSWORD_MD5")
echo "Password: $PASSWORD_MD5"
echo "Hash: $HASH_MD5"
echo "testuser:$HASH_MD5:18000:0:99999:7:::" > shadow_test_md5.txt
echo ""

echo "=== bcrypt ==="
HASH_BCRYPT=$(./gen_hash "$PASSWORD_BCRYPT" '$2b$05$saltsaltsaltsaltsalt12')
if [ $? -eq 0 ] && [ -n "$HASH_BCRYPT" ]; then
    echo "Password: $PASSWORD_BCRYPT"
    echo "Hash: $HASH_BCRYPT"
    echo "testuser:$HASH_BCRYPT:18000:0:99999:7:::" > shadow_test_bcrypt.txt
else
    echo "Error: Failed to generate bcrypt hash"
    exit 1
fi
echo ""

echo "=== SHA-256 ==="
HASH_SHA256=$(openssl passwd -5 -salt saltsalt "$PASSWORD_SHA256")
echo "Password: $PASSWORD_SHA256"
echo "Hash: $HASH_SHA256"
echo "testuser:$HASH_SHA256:18000:0:99999:7:::" > shadow_test_sha256.txt
echo ""

echo "=== SHA-512 ==="
HASH_SHA512=$(openssl passwd -6 -salt saltsalt "$PASSWORD_SHA512")
echo "Password: $PASSWORD_SHA512"
echo "Hash: $HASH_SHA512"
echo "testuser:$HASH_SHA512:18000:0:99999:7:::" > shadow_test_sha512.txt
echo ""

echo "=== yescrypt ==="
HASH_YESCRYPT=$(./gen_hash "$PASSWORD_YESCRYPT" '$y$j9T$n34PoBLMgFrQVl4Rn34Po/')
if [ $? -eq 0 ] && [ -n "$HASH_YESCRYPT" ]; then
    echo "Password: $PASSWORD_YESCRYPT"
    echo "Hash: $HASH_YESCRYPT"
    echo "testuser:$HASH_YESCRYPT:18000:0:99999:7:::" > shadow_test_yescrypt.txt
else
    echo "Error: Failed to generate yescrypt hash"
    exit 1
fi
echo ""

echo "Done! Generated shadow files:"
ls -l shadow_test_*.txt

echo ""
echo "Password summary:"
echo "  MD5:      $PASSWORD_MD5"
echo "  bcrypt:   $PASSWORD_BCRYPT"
echo "  SHA-256:  $PASSWORD_SHA256"
echo "  SHA-512:  $PASSWORD_SHA512"
echo "  yescrypt: $PASSWORD_YESCRYPT"
