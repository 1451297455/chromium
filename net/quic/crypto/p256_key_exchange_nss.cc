// Copyright (c) 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/quic/crypto/p256_key_exchange.h"

#include "base/logging.h"
#include "base/sys_byteorder.h"

namespace net {

namespace {

// Password used by |NewPrivateKey| to encrypt exported EC private keys.
// This is not used to provide any security, but to workaround NSS being
// unwilling to export unencrypted EC keys. Note that SPDY and ChannelID
// use the same approach.
const char kExportPassword[] = "";

// Convert StringPiece to vector of uint8
static std::vector<uint8> StringPieceToVector(base::StringPiece piece) {
  return std::vector<uint8>(piece.data(), piece.data() + piece.length());
}

}  // namespace

P256KeyExchange::P256KeyExchange(crypto::ECPrivateKey* key_pair,
                                 const uint8* public_key)
    : key_pair_(key_pair) {
  memcpy(public_key_, public_key, sizeof(public_key_));
}

P256KeyExchange::~P256KeyExchange() {
}

// static
P256KeyExchange* P256KeyExchange::New(base::StringPiece key) {
  if (key.size() < 2) {
    DLOG(INFO) << "Key pair is too small";
    return NULL;
  }

  const uint8* data = reinterpret_cast<const uint8*>(key.data());
  size_t size = static_cast<size_t>(data[0]) |
                (static_cast<size_t>(data[1]) << 8);
  key.remove_prefix(2);
  if (key.size() < size) {
    DLOG(INFO) << "Key pair does not contain key material";
    return NULL;
  }

  base::StringPiece private_piece(key.data(), size);
  key.remove_prefix(size);
  if (key.empty()) {
    DLOG(INFO) << "Key pair does not contain public key";
    return NULL;
  }

  base::StringPiece public_piece(key);

  scoped_ptr<crypto::ECPrivateKey> key_pair(
      crypto::ECPrivateKey::CreateFromEncryptedPrivateKeyInfo(
          kExportPassword,
          // TODO(thaidn): fix this interface to avoid copying secrets.
          StringPieceToVector(private_piece),
          StringPieceToVector(public_piece)));

  if (!key_pair.get()) {
    DLOG(INFO) << "Can't decrypt private key";
    return NULL;
  }

  // Perform some sanity checks on the public key.
  SECKEYPublicKey* public_key = key_pair->public_key();
  if (public_key->keyType != ecKey ||
      public_key->u.ec.publicValue.len != kUncompressedP256PointBytes ||
      !public_key->u.ec.publicValue.data ||
      public_key->u.ec.publicValue.data[0] != kUncompressedECPointForm) {
    DLOG(INFO) << "Key is invalid";
    return NULL;
  }

  // Ensure that the key is using the correct curve, i.e., NIST P-256.
  const SECOidData* oid_data = SECOID_FindOIDByTag(SEC_OID_SECG_EC_SECP256R1);
  if (!oid_data) {
    DLOG(INFO) << "Can't get P-256's OID";
    return NULL;
  }

  if (public_key->u.ec.DEREncodedParams.len != oid_data->oid.len + 2 ||
      !public_key->u.ec.DEREncodedParams.data ||
      public_key->u.ec.DEREncodedParams.data[0] != SEC_ASN1_OBJECT_ID ||
      public_key->u.ec.DEREncodedParams.data[1] != oid_data->oid.len ||
      memcmp(public_key->u.ec.DEREncodedParams.data + 2,
             oid_data->oid.data, oid_data->oid.len) != 0) {
    DLOG(INFO) << "Key is invalid";
  }

  return new P256KeyExchange(key_pair.release(),
                             public_key->u.ec.publicValue.data);
}

// static
std::string P256KeyExchange::NewPrivateKey() {
  scoped_ptr<crypto::ECPrivateKey> key_pair(crypto::ECPrivateKey::Create());

  if (!key_pair.get()) {
    DLOG(INFO) << "Can't generate new key pair";
    return std::string();
  }

  std::vector<uint8> private_key;
  if (!key_pair->ExportEncryptedPrivateKey(kExportPassword,
                                           1 /* iteration */,
                                           &private_key)) {
    DLOG(INFO) << "Can't export private key";
    return std::string();
  }

  // NSS lacks the ability to import an ECC private key without
  // also importing the public key, so it is necessary to also
  // store the public key.
  std::vector<uint8> public_key;
  if (!key_pair->ExportPublicKey(&public_key)) {
    DLOG(INFO) << "Can't export public key";
    return std::string();
  }

  // TODO(thaidn): determine how large encrypted private key can be
  uint16 private_key_size = private_key.size();
  const size_t result_size = sizeof(private_key_size) +
                             private_key_size +
                             public_key.size();
  std::vector<char> result(result_size);
  char* resultp = &result[0];
  // Export the key string.
  // The first two bytes are the private key's size in little endian.
  private_key_size = base::ByteSwapToLE16(private_key_size);
  memcpy(resultp, &private_key_size, sizeof(private_key_size));
  resultp += sizeof(private_key_size);
  memcpy(resultp, &private_key[0], private_key.size());
  resultp += private_key.size();
  memcpy(resultp, &public_key[0], public_key.size());

  return std::string(&result[0], result_size);
}

bool P256KeyExchange::CalculateSharedKey(
    const base::StringPiece& peer_public_value,
    std::string* out_result) const {
  if (peer_public_value.size() != kUncompressedP256PointBytes ||
      peer_public_value[0] != kUncompressedECPointForm) {
    DLOG(INFO) << "Peer public value is invalid";
    return false;
  }

  DCHECK(key_pair_.get());
  DCHECK(key_pair_->public_key());

  SECKEYPublicKey peer_public_key;
  memset(&peer_public_key, 0, sizeof(peer_public_key));

  peer_public_key.keyType = ecKey;
  // Both sides of a ECDH key exchange need to use the same EC params.
  peer_public_key.u.ec.DEREncodedParams.len =
      key_pair_->public_key()->u.ec.DEREncodedParams.len;
  peer_public_key.u.ec.DEREncodedParams.data =
      key_pair_->public_key()->u.ec.DEREncodedParams.data;

  peer_public_key.u.ec.publicValue.type = siBuffer;
  peer_public_key.u.ec.publicValue.data =
      reinterpret_cast<uint8*>(const_cast<char*>(peer_public_value.data()));
  peer_public_key.u.ec.publicValue.len = peer_public_value.size();

  // The NSS function performing ECDH key exchange is PK11_PubDeriveWithKDF.
  // As this function is used for SSL/TLS's ECDH key exchanges it has many
  // arguments, most of which are not required in QUIC.
  // Key derivation function CKD_NULL is used because the return value of
  // |CalculateSharedKey| is the actual ECDH shared key, not any derived keys
  // from it.
  crypto::ScopedPK11SymKey premaster_secret(
      PK11_PubDeriveWithKDF(
          key_pair_->key(),
          &peer_public_key,
          PR_FALSE,
          NULL,
          NULL,
          CKM_ECDH1_DERIVE, /* mechanism */
          CKM_GENERIC_SECRET_KEY_GEN, /* target */
          CKA_DERIVE,
          0,
          CKD_NULL, /* kdf */
          NULL,
          NULL));

  if (!premaster_secret.get()) {
    DLOG(INFO) << "Can't derive ECDH shared key";
    return false;
  }

  if (PK11_ExtractKeyValue(premaster_secret.get()) != SECSuccess) {
    DLOG(INFO) << "Can't extract raw ECDH shared key";
    return false;
  }

  SECItem* key_data = PK11_GetKeyData(premaster_secret.get());
  if (!key_data || !key_data->data || key_data->len != kP256FieldBytes) {
    DLOG(INFO) << "ECDH shared key is invalid";
    return false;
  }

  out_result->assign(reinterpret_cast<char*>(key_data->data), key_data->len);
  return true;
}

base::StringPiece P256KeyExchange::public_value() const {
  return base::StringPiece(reinterpret_cast<const char*>(public_key_),
                           sizeof(public_key_));
}

CryptoTag P256KeyExchange::tag() const {
  return kP256;
}

}  // namespace net

