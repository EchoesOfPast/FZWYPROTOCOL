#pragma once

#include <QByteArray>

namespace crypto {

QByteArray md5Hex(const QByteArray &in);
QByteArray rand16();

QByteArray aesCbcEncrypt(const QByteArray &key, QByteArray iv, const QByteArray &data);
QByteArray aesCbcDecrypt(const QByteArray &key, QByteArray iv, const QByteArray &data);

class RsaKeyPair {
public:
    RsaKeyPair() = default;
    ~RsaKeyPair();
    RsaKeyPair(const RsaKeyPair &) = delete;
    RsaKeyPair &operator=(const RsaKeyPair &) = delete;
    RsaKeyPair(RsaKeyPair &&o) noexcept;
    RsaKeyPair &operator=(RsaKeyPair &&o) noexcept;

    bool ok() const { return m_hKey != nullptr; }
    QByteArray publicKeySpkiDer() const;
    QByteArray decryptPkcs1(const QByteArray &cipher) const;

private:
    friend RsaKeyPair rsaGenerate1024();
    void *m_hKey = nullptr;  // BCRYPT_KEY_HANDLE
    QByteArray m_spkiDer;
};

RsaKeyPair rsaGenerate1024();

QByteArray rsaEncryptPkcs1WithSpki(const QByteArray &spkiDer, const QByteArray &data);

}  // namespace crypto
