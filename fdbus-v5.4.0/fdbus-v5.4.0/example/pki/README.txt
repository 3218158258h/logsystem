openssl genrsa -aes256 -passout pass:123456 -out ca_rsa_private.pem 2048
openssl req -new -x509 -days 365 -key ca_rsa_private.pem -passin pass:123456 -out ca.crt -subj "/C=CN/ST=GD/L=SZ/O=COM/OU=NSP/CN=CA/emailAddress=youremail@qq.com"

openssl genrsa -aes256 -passout pass:server -out server_rsa_private.pem 2048
openssl req -new -key server_rsa_private.pem -passin pass:server -out server.csr -subj "/C=CN/ST=GD/L=SZ/O=COM/OU=NSP/CN=SERVER/emailAddress=youremail@qq.com"

openssl x509 -req -days 365 -in server.csr -CA ca.crt -CAkey ca_rsa_private.pem -passin pass:123456 -CAcreateserial -out server.crt

test:
> fdbobjtest.exe -s my-server-name -p server.crt -v server_rsa_private.pem
Enter PEM pass phrase:server
> fdbobjtest.exe -c my-server-name -r ca.crt
