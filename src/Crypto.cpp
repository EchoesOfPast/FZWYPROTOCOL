#include "Crypto.h"

#include <windows.h>
#include <bcrypt.h>
#include <ncrypt.h>

#include <QRandomGenerator>



namespace crypto {

QByteArray md5Hex(const QByteArray &in) {
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_MD5_ALGORITHM, nullptr, 0) != 0)
        return {};
    BCRYPT_HASH_HANDLE hHash = nullptr;
    if (BCryptCreateHash(hAlg, &hHash, nullptr, 0, nullptr, 0, 0) != 0) {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return {};
    }
    if (BCryptHashData(hHash, reinterpret_cast<PUCHAR>(const_cast<char*>(in.constData())),
                       static_cast<ULONG>(in.size()), 0) != 0) {
        BCryptDestroyHash(hHash);
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return {};
    }
    BYTE out[16];
    if (BCryptFinishHash(hHash, out, 16, 0) != 0) {
        BCryptDestroyHash(hHash);
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return {};
    }
    BCryptDestroyHash(hHash);
    BCryptCloseAlgorithmProvider(hAlg, 0);
    return QByteArray(reinterpret_cast<char*>(out), 16).toHex();
}

QByteArray rand16() {
    static const char kAlph[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    QByteArray s(16, 0);
    for (auto &ch : s)
        ch = kAlph[QRandomGenerator::global()->bounded(62)];
    return s;
}

static QByteArray aesCrypt(bool encrypt, const QByteArray &key, QByteArray iv,
                           const QByteArray &data) {
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_AES_ALGORITHM, nullptr, 0) != 0)
        return {};
    if (BCryptSetProperty(hAlg, BCRYPT_CHAINING_MODE,
                          reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_CBC)),
                          sizeof(BCRYPT_CHAIN_MODE_CBC), 0) != 0) {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return {};
    }
    BCRYPT_KEY_HANDLE hKey = nullptr;
    if (BCryptGenerateSymmetricKey(hAlg, &hKey, nullptr, 0,
                                   reinterpret_cast<PUCHAR>(const_cast<char*>(key.constData())),
                                   static_cast<ULONG>(key.size()), 0) != 0) {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return {};
    }
    QByteArray out;
    ULONG outLen = 0, done = 0;
    if (encrypt) {
        if (BCryptEncrypt(hKey, reinterpret_cast<PUCHAR>(const_cast<char*>(data.constData())),
                          static_cast<ULONG>(data.size()), nullptr,
                          reinterpret_cast<PUCHAR>(iv.data()), static_cast<ULONG>(iv.size()),
                          nullptr, 0, &outLen, BCRYPT_BLOCK_PADDING) != 0)
            goto cleanup;
        out.resize(static_cast<int>(outLen));
        if (BCryptEncrypt(hKey, reinterpret_cast<PUCHAR>(const_cast<char*>(data.constData())),
                          static_cast<ULONG>(data.size()), nullptr,
                          reinterpret_cast<PUCHAR>(iv.data()), static_cast<ULONG>(iv.size()),
                          reinterpret_cast<PUCHAR>(out.data()), outLen, &done,
                          BCRYPT_BLOCK_PADDING) != 0) {
            out.clear();
            goto cleanup;
        }
        out.resize(static_cast<int>(done));
    } else {
        if (BCryptDecrypt(hKey, reinterpret_cast<PUCHAR>(const_cast<char*>(data.constData())),
                          static_cast<ULONG>(data.size()), nullptr,
                          reinterpret_cast<PUCHAR>(iv.data()), static_cast<ULONG>(iv.size()),
                          nullptr, 0, &outLen, BCRYPT_BLOCK_PADDING) != 0)
            goto cleanup;
        out.resize(static_cast<int>(outLen));
        if (BCryptDecrypt(hKey, reinterpret_cast<PUCHAR>(const_cast<char*>(data.constData())),
                          static_cast<ULONG>(data.size()), nullptr,
                          reinterpret_cast<PUCHAR>(iv.data()), static_cast<ULONG>(iv.size()),
                          reinterpret_cast<PUCHAR>(out.data()), outLen, &done,
                          BCRYPT_BLOCK_PADDING) != 0) {
            out.clear();
            goto cleanup;
        }
        out.resize(static_cast<int>(done));
    }
cleanup:
    BCryptDestroyKey(hKey);
    BCryptCloseAlgorithmProvider(hAlg, 0);
    return out;
}

QByteArray aesCbcEncrypt(const QByteArray &key, QByteArray iv, const QByteArray &data) {
    return aesCrypt(true, key, std::move(iv), data);
}

QByteArray aesCbcDecrypt(const QByteArray &key, QByteArray iv, const QByteArray &data) {
    return aesCrypt(false, key, std::move(iv), data);
}

static QByteArray derLen(int n) {
    if (n < 128)
        return QByteArray(1, static_cast<char>(n));
    QByteArray b;
    while (n) {
        b.prepend(static_cast<char>(n & 0xff));
        n >>= 8;
    }
    return QByteArray(1, static_cast<char>(0x80 | b.size())) + b;
}

static QByteArray derTlv(char tag, const QByteArray &content) {
    return QByteArray(1, tag) + derLen(static_cast<int>(content.size())) + content;
}

static QByteArray derInt(QByteArray v) {
    while (v.size() > 1 && static_cast<uchar>(v[0]) == 0)
        v.remove(0, 1);
    if (v.isEmpty())
        v = QByteArray(1, '\0');
    if (static_cast<uchar>(v[0]) & 0x80)
        v.prepend('\0');
    return derTlv(0x02, v);
}

static QByteArray derSpkiFromModExp(const QByteArray &mod, const QByteArray &exp) {
    const QByteArray rsaOid = QByteArray::fromHex("06092a864886f70d010101");
    QByteArray algid = derTlv(0x30, rsaOid + derTlv(0x05, ""));
    QByteArray rsaPub = derTlv(0x30, derInt(mod) + derInt(exp));
    QByteArray bitStr = derTlv(0x03, QByteArray(1, '\0') + rsaPub);
    return derTlv(0x30, algid + bitStr);
}

static bool derReadTlv(const char *&p, const char *end, char expectTag, const char *&content,
                       size_t &len) {
    if (p >= end || *p != expectTag)
        return false;
    ++p;
    if (p >= end)
        return false;
    auto lb = static_cast<uchar>(*p++);
    size_t n = 0;
    if (lb < 0x80) {
        n = lb;
    } else {
        int cnt = lb & 0x7f;
        if (cnt == 0 || cnt > 4 || p + cnt > end)
            return false;
        for (int i = 0; i < cnt; ++i)
            n = (n << 8) | static_cast<uchar>(*p++);
    }
    if (p + n > end)
        return false;
    content = p;
    len = n;
    p += n;
    return true;
}

static bool derSpkiToModExp(const QByteArray &der, QByteArray &mod, QByteArray &exp) {
    const char *p = der.constData();
    const char *end = p + der.size();
    const char *c = nullptr;
    size_t len = 0;
    if (!derReadTlv(p, end, 0x30, c, len))  // SPKI SEQUENCE
        return false;
    const char *q = c;
    const char *qend = c + len;
    if (!derReadTlv(q, qend, 0x30, c, len))  // algid SEQUENCE (skip)
        return false;
    if (!derReadTlv(q, qend, 0x03, c, len))  // BIT STRING
        return false;
    if (len < 1 || c[0] != 0)
        return false;
    const char *r = c + 1;
    const char *rend = c + len;
    if (!derReadTlv(r, rend, 0x30, c, len))  // RSAPublicKey SEQUENCE
        return false;
    r = c;
    rend = c + len;
    if (!derReadTlv(r, rend, 0x02, c, len))  // modulus
        return false;
    mod = QByteArray(c, static_cast<int>(len));
    while (mod.size() > 1 && mod[0] == 0)
        mod.remove(0, 1);
    if (!derReadTlv(r, rend, 0x02, c, len))  // exponent
        return false;
    exp = QByteArray(c, static_cast<int>(len));
    while (exp.size() > 1 && exp[0] == 0)
        exp.remove(0, 1);
    return !mod.isEmpty() && !exp.isEmpty();
}

RsaKeyPair::~RsaKeyPair() {
    if (m_hKey)
        BCryptDestroyKey(static_cast<BCRYPT_KEY_HANDLE>(m_hKey));
}

RsaKeyPair::RsaKeyPair(RsaKeyPair &&o) noexcept
    : m_hKey(o.m_hKey), m_spkiDer(std::move(o.m_spkiDer)) {
    o.m_hKey = nullptr;
}

RsaKeyPair &RsaKeyPair::operator=(RsaKeyPair &&o) noexcept {
    if (this != &o) {
        if (m_hKey)
            BCryptDestroyKey(static_cast<BCRYPT_KEY_HANDLE>(m_hKey));
        m_hKey = o.m_hKey;
        m_spkiDer = std::move(o.m_spkiDer);
        o.m_hKey = nullptr;
    }
    return *this;
}

QByteArray RsaKeyPair::publicKeySpkiDer() const { return m_spkiDer; }

QByteArray RsaKeyPair::decryptPkcs1(const QByteArray &cipher) const {
    if (!m_hKey)
        return {};
    BCRYPT_PKCS1_PADDING_INFO padInfo{};
    padInfo.pszAlgId = nullptr;
    ULONG outLen = 0, done = 0;
    if (BCryptDecrypt(static_cast<BCRYPT_KEY_HANDLE>(m_hKey),
                      reinterpret_cast<PUCHAR>(const_cast<char*>(cipher.constData())),
                      static_cast<ULONG>(cipher.size()), &padInfo, nullptr, 0, nullptr, 0,
                      &outLen, BCRYPT_PAD_PKCS1) != 0)
        return {};
    QByteArray out(static_cast<int>(outLen), 0);
    if (BCryptDecrypt(static_cast<BCRYPT_KEY_HANDLE>(m_hKey),
                      reinterpret_cast<PUCHAR>(const_cast<char*>(cipher.constData())),
                      static_cast<ULONG>(cipher.size()), &padInfo, nullptr, 0,
                      reinterpret_cast<PUCHAR>(out.data()), outLen, &done,
                      BCRYPT_PAD_PKCS1) != 0)
        return {};
    out.resize(static_cast<int>(done));
    return out;
}

RsaKeyPair rsaGenerate1024() {
    RsaKeyPair kp;
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_RSA_ALGORITHM, nullptr, 0) != 0)
        return kp;
    BCRYPT_KEY_HANDLE hKey = nullptr;
    if (BCryptGenerateKeyPair(hAlg, &hKey, 1024, 0) != 0 ||
        BCryptFinalizeKeyPair(hKey, 0) != 0) {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return kp;
    }
    ULONG len = 0;
    if (BCryptExportKey(hKey, nullptr, BCRYPT_RSAPUBLIC_BLOB, nullptr, 0, &len, 0) != 0)
        goto fail;
    {
        QByteArray blob(static_cast<int>(len), 0);
        if (BCryptExportKey(hKey, nullptr, BCRYPT_RSAPUBLIC_BLOB,
                            reinterpret_cast<PUCHAR>(blob.data()), len, &len, 0) != 0)
            goto fail;
        auto *hdr = reinterpret_cast<const BCRYPT_RSAKEY_BLOB*>(blob.constData());
        const char *p = blob.constData() + sizeof(BCRYPT_RSAKEY_BLOB);
        QByteArray exp(p, static_cast<int>(hdr->cbPublicExp));
        p += hdr->cbPublicExp;
        QByteArray mod(p, static_cast<int>(hdr->cbModulus));
        kp.m_spkiDer = derSpkiFromModExp(mod, exp);
    }
    kp.m_hKey = hKey;
    BCryptCloseAlgorithmProvider(hAlg, 0);
    return kp;
fail:
    BCryptDestroyKey(hKey);
    BCryptCloseAlgorithmProvider(hAlg, 0);
    return kp;
}

QByteArray rsaEncryptPkcs1WithSpki(const QByteArray &spkiDer, const QByteArray &data) {
    QByteArray mod, exp;
    if (!derSpkiToModExp(spkiDer, mod, exp)) {
        return {};
    }

    // Public keys imported via BCryptImportKey support only signature verification,
    // not encryption (CNG limitation) - import via NCrypt and use NCryptEncrypt.
    NCRYPT_PROV_HANDLE hProv = 0;
    if (NCryptOpenStorageProvider(&hProv, MS_KEY_STORAGE_PROVIDER, 0) != 0) {
        return {};
    }
    QByteArray blob;
    BCRYPT_RSAKEY_BLOB hdr{};
    hdr.Magic = BCRYPT_RSAPUBLIC_MAGIC;
    hdr.BitLength = static_cast<ULONG>(mod.size()) * 8;
    hdr.cbPublicExp = static_cast<ULONG>(exp.size());
    hdr.cbModulus = static_cast<ULONG>(mod.size());
    blob.append(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
    blob.append(exp);
    blob.append(mod);

    NCRYPT_KEY_HANDLE hKey = 0;
    QByteArray out;
    SECURITY_STATUS ss = NCryptImportKey(hProv, 0, BCRYPT_RSAPUBLIC_BLOB, nullptr, &hKey,
                                         reinterpret_cast<PBYTE>(blob.data()),
                                         static_cast<DWORD>(blob.size()), 0);
    if (ss != 0) {
        goto cleanup;
    }
    {
        BCRYPT_PKCS1_PADDING_INFO padInfo{};
        padInfo.pszAlgId = nullptr;
        DWORD outLen = 0, done = 0;
        ss = NCryptEncrypt(hKey, reinterpret_cast<PBYTE>(const_cast<char*>(data.constData())),
                           static_cast<DWORD>(data.size()), &padInfo, nullptr, 0, &outLen,
                           NCRYPT_PAD_PKCS1_FLAG);
        if (ss != 0) {
            goto cleanup;
        }
        out.resize(static_cast<int>(outLen));
        ss = NCryptEncrypt(hKey, reinterpret_cast<PBYTE>(const_cast<char*>(data.constData())),
                           static_cast<DWORD>(data.size()), &padInfo,
                           reinterpret_cast<PBYTE>(out.data()), outLen, &done,
                           NCRYPT_PAD_PKCS1_FLAG);
        if (ss != 0) {
            out.clear();
            goto cleanup;
        }
        out.resize(static_cast<int>(done));
    }
cleanup:
    if (hKey)
        NCryptFreeObject(hKey);
    if (hProv)
        NCryptFreeObject(hProv);
    return out;
}

}  // namespace crypto
