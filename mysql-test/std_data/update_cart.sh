## Usage: Generate CA, server, and client certificates for TLS/SSL testing
## Scenario: Creates a self-signed CA, server certificates with/without SAN validation, and client certificate for mutual authentication testing

# Create clean environment
rm -rf newcerts
mkdir newcerts && cd newcerts

# Generate CA private key and certificate
openssl genrsa 2048 > ca-key.pem
openssl req -new -x509 -nodes -days 3650 \
  -key ca-key.pem -out cacert.pem \
  -subj "/C=SE/ST=Stockholm/L=Stockholm/O=Oracle/OU=MySQL/CN=CA"

openssl req -new -x509 -nodes -days 3650 \
  -key ca-key.pem -out ca-cert-verify.pem \
  -subj "/C=SE/ST=Stockholm/L=Stockholm/O=Oracle/OU=MySQL/CN=CA"

# Generate server certificate with SAN extension
echo "subjectAltName=DNS:localhost" > extfile.cnf
openssl req -newkey rsa:2048 -days 3650 \
  -nodes -keyout server-key.pem -out server-req.pem \
  -subj "/C=SE/ST=Stockholm/L=Stockholm/O=Oracle/OU=MySQL/CN=nonexistent.example.com"
openssl rsa -in server-key.pem -out server-key.pem
openssl x509 -req -in server-req.pem -days 3650 \
  -CA ca-cert-verify.pem -CAkey ca-key.pem -set_serial 01 -out server-cert.pem -extfile extfile.cnf

# Generate server certificate with invalid OU for verification failure test
openssl req -newkey rsa:2048 -days 3650 \
  -nodes -keyout server-key-verify-fail.pem -out server-req.pem \
  -subj "/C=SE/ST=Stockholm/L=Stockholm/O=Oracle/OU=localhost/CN=nonexistent.example.com"
openssl rsa -in server-key-verify-fail.pem -out server-key-verify-fail.pem
openssl x509 -req -in server-req.pem -days 3650 \
  -CA ca-cert-verify.pem -CAkey ca-key.pem -set_serial 01 -out server-cert-verify-fail.pem

# Generate valid server certificate for verification success test
openssl req -newkey rsa:2048 -days 3650 \
  -nodes -keyout server-key-verify-pass.pem -out server-req.pem \
  -subj "/C=SE/ST=Stockholm/L=Stockholm/O=Oracle/OU=MySQL/CN=nonexistent.example.com"
openssl rsa -in server-key-verify-pass.pem -out server-key-verify-pass.pem
openssl x509 -req -in server-req.pem -days 3650 \
  -CA ca-cert-verify.pem -CAkey ca-key.pem -set_serial 01 -out server-cert-verify-pass.pem -extfile extfile.cnf

# Generate client certificate for mutual TLS authentication
openssl req -newkey rsa:2048 -days 3650 \
  -nodes -keyout client-key.pem -out client-req.pem \
  -subj "/C=SE/ST=Stockholm/L=Stockholm/O=Oracle/OU=MySQL/CN=Client"
openssl rsa -in client-key.pem -out client-key.pem
openssl x509 -req -in client-req.pem -days 3650 \
  -CA ca-cert-verify.pem -CAkey ca-key.pem -set_serial 02 -out client-cert.pem

# Verify certificate validity and chain trust
openssl verify -CAfile ca-cert-verify.pem server-cert.pem client-cert.pem