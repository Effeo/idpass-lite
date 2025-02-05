/*
 * Copyright (C) 2020 Newlogic Pte. Ltd.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "idpass.h"
#include "bin16.h"
#include "CCertificate.h"
#include "proto/api/api.pb.h"
#include "proto/idpasslite/idpasslite.pb.h"
#include "sodium.h"

#include <gtest/gtest.h>

#include <chrono>
#include <ctime>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define _CRT_INTERNAL_NONSTDC_NAMES 1
#include <sys/stat.h>

#if !defined(S_ISREG) && defined(S_IFMT) && defined(S_IFREG)
#define S_ISREG(m) (((m)&S_IFMT) == S_IFREG)
#endif

#if !defined(S_ISDIR) && defined(S_IFMT) && defined(S_IFDIR)
#define S_ISDIR(m) (((m)&S_IFMT) == S_IFDIR)
#endif
#endif

char const* datapath = "data/";

class TestCases : public testing::Test
{
protected:
    void* ctx;
    unsigned char* m_enc;
    unsigned char* m_sig;
    unsigned char* m_ver;
    CCertificate* m_rootCert1;
    api::Ident m_ident;

    void SetUp() override
    {
        std::string filename = std::string(datapath) + "manny1.bmp";
        std::ifstream photofile(filename, std::ios::binary);
        std::vector<char> photo(std::istreambuf_iterator<char>{photofile}, {});

        m_ident.set_surname("Pacquiao");
        m_ident.set_givenname("Manny");
        m_ident.set_placeofbirth("Kibawe, Bukidnon");
        m_ident.set_pin("12345");
        m_ident.mutable_dateofbirth()->set_year(1978);
        m_ident.mutable_dateofbirth()->set_month(12);
        m_ident.mutable_dateofbirth()->set_day(17);
        m_ident.set_photo(photo.data(), photo.size());

        m_enc = new unsigned char[32];
        m_sig = new unsigned char[64];
        m_ver = new unsigned char[32];
        m_rootCert1 = new CCertificate();

        api::KeySet cryptoKeys;

        idpass_lite_generate_secret_signature_keypair(m_ver, 32, m_sig, 64);
        idpass_lite_generate_encryption_key(m_enc, 32);

        cryptoKeys.set_encryptionkey(m_enc, 32);
        cryptoKeys.set_signaturekey(m_sig, 64);
        api::byteArray* verkey = cryptoKeys.add_verificationkeys();
        verkey->set_typ(api::byteArray_Typ_ED25519PUBKEY);
        verkey->set_val(m_ver, 32);

        std::vector<unsigned char> keysetbuf;

        keysetbuf.resize(cryptoKeys.ByteSizeLong());
        cryptoKeys.SerializeToArray(keysetbuf.data(),
                                    keysetbuf.size());

        CCertificate rootCert2;
        CCertificate rootCert3;

        api::Certificates rootCertificates;
        idpass::Certificate* cert1 = rootCertificates.add_cert();
        idpass::Certificate* cert2 = rootCertificates.add_cert();
        idpass::Certificate* cert3 = rootCertificates.add_cert();
        cert1->CopyFrom(m_rootCert1->getValue());
        cert2->CopyFrom(rootCert2.getValue());
        cert3->CopyFrom(rootCert3.getValue());
        std::vector<unsigned char> rootcertsbuf(rootCertificates.ByteSizeLong());
        rootCertificates.SerializeToArray(rootcertsbuf.data(), rootcertsbuf.size());

        ctx = idpass_lite_init(
            keysetbuf.data(), keysetbuf.size(), 
            rootcertsbuf.data(), rootcertsbuf.size());

        ASSERT_TRUE(ctx != nullptr);
    }

    void TearDown() override
    {
        delete[] m_enc;
        delete[] m_sig;
        delete[] m_ver;
        delete m_rootCert1;
        idpass_lite_freemem(ctx, ctx);
    }
};

TEST_F(TestCases, card_integrity_test)
{
    std::string inputfile = std::string(datapath) + "manny1.bmp";
    std::ifstream f1(inputfile, std::ios::binary);
    std::vector<char> photo(std::istreambuf_iterator<char>{f1}, {});

    std::uint64_t vizflags = DETAIL_SURNAME | DETAIL_PLACEOFBIRTH;
    unsigned char ioctlcmd[9];
    ioctlcmd[0] = IOCTL_SET_ACL;
    std::memcpy(&ioctlcmd[1], &vizflags, 8);

    idpass_lite_ioctl(ctx, nullptr, ioctlcmd, sizeof ioctlcmd);

    idpass::Pair* privextra = m_ident.add_privextra();
    privextra->set_key("color");
    privextra->set_value("blue");

    std::vector<unsigned char> buf(m_ident.ByteSizeLong());
    m_ident.SerializeToArray(buf.data(), buf.size());
    
    int cards_len;
    unsigned char* cards
        = idpass_lite_create_card_with_face(ctx, &cards_len, buf.data(), buf.size());

    ASSERT_TRUE(cards != nullptr); 

    int details_len = 0;
    unsigned char* details = idpass_lite_verify_card_with_face(ctx,
                                                               &details_len,
                                                               cards,
                                                               cards_len,
                                                               photo.data(),
                                                               photo.size());

    int cert_count = idpass_lite_verify_certificate(ctx, cards, cards_len);
    // The idpass_lite_verify_certificate function returns either
    // < 0, 0 or > 0 integer.
    //
    // The 0 means the QR code ID card has no attached certificates
    // but nevertheless the card's signature is verified against 
    // the context's ed25519 private key
    // Any error conditions either in certificate validation
    // or QR code ID card signature verification shall return
    // negative number. In summary:
    // 
    // 0    means the card has no attached certificate and 
    //      the signature is verified against caller context
    // 2    means the card has 2 attached certificates and the signature
    //      is verified against the caller context(or leaf cert)
    // < 0  means an error either in certificate validation or 
    //      in signature verification
    ASSERT_TRUE(cert_count != 0); 

    CCertificate child0;
    CCertificate child1(m_sig, 64);
    m_rootCert1->Sign(child0);
    child0.Sign(child1);

    api::Certificates intermediateCertificates;
    idpass::Certificate* c1 = intermediateCertificates.add_cert();
    c1->CopyFrom(child0.getValue());
    idpass::Certificate* c2 = intermediateCertificates.add_cert();
    c2->CopyFrom(child1.getValue());

    std::vector<unsigned char> intermedcerts_buf(intermediateCertificates.ByteSizeLong());

    intermediateCertificates.SerializeToArray(intermedcerts_buf.data(),
                                              intermedcerts_buf.size());

    int n = idpass_lite_add_certificates(
        ctx, intermedcerts_buf.data(), intermedcerts_buf.size());

    int card2_len;
    unsigned char* card2
        = idpass_lite_create_card_with_face(ctx, &card2_len, buf.data(), buf.size());

    cert_count = idpass_lite_verify_certificate(ctx, card2, card2_len);
    // The 2 means there are 2 validated intermediate certificactes
    // and card's signature is verified against the context's 
    // ed25519 private key
    ASSERT_EQ(cert_count, 2);

    ASSERT_EQ(0, idpass_lite_verify_card_signature(ctx, card2, card2_len, 1));
}

TEST_F(TestCases, create_card_with_certificates_content_tampering)
{
    std::string inputfile = std::string(datapath) + "manny1.bmp";
    std::ifstream f1(inputfile, std::ios::binary);
    std::vector<char> photo(std::istreambuf_iterator<char>{f1}, {});

    std::uint64_t vizFlags = DETAIL_SURNAME | DETAIL_PLACEOFBIRTH;
    unsigned char ioctlcmd[9];
    ioctlcmd[0] = IOCTL_SET_ACL;
    std::memcpy(&ioctlcmd[1], &vizFlags, 8);

    idpass_lite_ioctl(ctx, nullptr, ioctlcmd, sizeof ioctlcmd);

    idpass::Pair* privextra = m_ident.add_privextra();
    privextra->set_key("color");
    privextra->set_value("blue");

    std::vector<unsigned char> buf(m_ident.ByteSizeLong());
    m_ident.SerializeToArray(buf.data(), buf.size());
    
    int len;
    unsigned char* cards
        = idpass_lite_create_card_with_face(ctx, &len, buf.data(), buf.size());

    ASSERT_TRUE(cards != nullptr);

    idpass::IDPassCards idpassCards;
    ASSERT_TRUE(idpassCards.ParseFromArray(cards, len));

    idpass::PublicSignedIDPassCard publicRegion = idpassCards.publiccard();

    ASSERT_STREQ(publicRegion.details().surname().c_str(), "Pacquiao");
    ASSERT_STREQ(publicRegion.details().placeofbirth().c_str(),
                 "Kibawe, Bukidnon");

    idpass::PublicSignedIDPassCard publicRegion_tampered;
    idpass::CardDetails details_tampered;
    details_tampered.set_placeofbirth("Kibawe,Bukidnon");
    publicRegion_tampered.mutable_details()->CopyFrom(details_tampered);

    idpassCards.mutable_publiccard()->CopyFrom(publicRegion_tampered);

    int n = idpassCards.ByteSizeLong();
    std::vector<unsigned char> tampered(n);
    ASSERT_TRUE(idpassCards.SerializeToArray(tampered.data(), n));

    int details_len = 0;
    unsigned char* details = idpass_lite_verify_card_with_face(ctx,
                                                               &details_len,
                                                               tampered.data(),
                                                               tampered.size(),
                                                               photo.data(),
                                                               photo.size());

    ASSERT_TRUE(details == nullptr);
}

TEST_F(TestCases, idpass_lite_create_card_with_face_certificates)
{
    std::string filename = std::string(datapath) + "manny1.bmp";
    std::ifstream photofile(filename, std::ios::binary);
    std::vector<char> photo(std::istreambuf_iterator<char>{photofile}, {});

    std::vector<unsigned char> ident_buf(m_ident.ByteSizeLong());
    m_ident.SerializeToArray(ident_buf.data(), ident_buf.size());

    int n;

    CCertificate child0;
    CCertificate child1(m_sig, 64);
    m_rootCert1->Sign(child0);
    child0.Sign(child1);

    api::Certificates intermediateCertificates;
    idpass::Certificate* c1 = intermediateCertificates.add_cert();
    c1->CopyFrom(child0.getValue());
    idpass::Certificate* c2 = intermediateCertificates.add_cert();
    c2->CopyFrom(child1.getValue());

    std::vector<unsigned char> intermedcerts_buf(intermediateCertificates.ByteSizeLong());

    intermediateCertificates.SerializeToArray(intermedcerts_buf.data(),
                                              intermedcerts_buf.size());

    n = idpass_lite_add_certificates(
        ctx, intermedcerts_buf.data(), intermedcerts_buf.size());

    ASSERT_TRUE(n == 0); // 0 means no error

    /*
    Create an ID for the person details in the ident structure. This
    returns a deserialized idpass::IDPassCards protobuf message.
    */

    int buf_len = 0;
    unsigned char* buf = idpass_lite_create_card_with_face(
        ctx, &buf_len, ident_buf.data(), ident_buf.size());

    ASSERT_TRUE(buf != nullptr);

    /*
    Construct idpass::IDPassCards from the returned byte[]
    */

    idpass::IDPassCards cards;
    ASSERT_TRUE(cards.ParseFromArray(buf, buf_len));

    /*
    List certificates
    */

    ASSERT_TRUE(cards.certificates_size() == 2);

    std::vector<idpass::Certificate> chain;

    for (auto& c : cards.certificates()) {
        chain.push_back(c);
    }

    ASSERT_TRUE(std::memcmp(chain[0].pubkey().data(), child0.m_pk.data(), 32) == 0);
    ASSERT_TRUE(std::memcmp(chain[1].pubkey().data(), child1.m_pk.data(), 32) == 0);

    /*
    Present the user's ID and if the face match, will return
    the person's details.
    */

    int details_len = 0;
    unsigned char* details = idpass_lite_verify_card_with_face(
        ctx, &details_len, buf, buf_len, photo.data(), photo.size());

    ASSERT_TRUE(details != nullptr);
}

TEST_F(TestCases, cannot_add_intermed_cert_without_rootcert)
{
    CCertificate cert0;
    CCertificate cert1;
    
    cert1.setPublicKey(m_ver, 32); // very important
    cert0.Sign(cert1);
    m_rootCert1->Sign(cert1);

    api::Certificates intermedcerts;
    intermedcerts.add_cert()->CopyFrom(cert0.getValue());
    intermedcerts.add_cert()->CopyFrom(cert1.getValue());

    std::vector<unsigned char> buf(intermedcerts.ByteSizeLong());
    intermedcerts.SerializeToArray(buf.data(), buf.size());

    api::KeySet keyset;
    unsigned char enc[32];
    unsigned char sig[64];
    unsigned char ver[32];

    idpass_lite_generate_secret_signature_keypair(ver, 32, sig, 64);
    idpass_lite_generate_encryption_key(enc, 32);

    keyset.set_encryptionkey(enc, 32);
    keyset.set_signaturekey(sig, 64);
    api::byteArray* verkey = keyset.add_verificationkeys();
    verkey->set_typ(api::byteArray_Typ_ED25519PUBKEY);
    verkey->set_val(ver, 32);

    std::vector<unsigned char> _keyset(keyset.ByteSizeLong());
    keyset.SerializeToArray(_keyset.data(), _keyset.size());

    void* context
        = idpass_lite_init(_keyset.data(), _keyset.size(), nullptr, 0);

    // have surname visible in public region so we can do quick check below
    std::uint64_t vizflags = DETAIL_SURNAME | DETAIL_PLACEOFBIRTH; 
    unsigned char ioctlcmd[9];
    ioctlcmd[0] = IOCTL_SET_ACL;
    std::memcpy(&ioctlcmd[1], &vizflags, 8);
    idpass_lite_ioctl(context, nullptr, ioctlcmd, sizeof ioctlcmd);


    // should still initialize even without rootcerts
    ASSERT_TRUE(context != nullptr);     

    // cannot add intermed certs without rootcerts
    ASSERT_TRUE(0 != idpass_lite_add_certificates(context, 
        buf.data(), buf.size())); 

    std::vector<unsigned char> _ident(m_ident.ByteSizeLong());
    m_ident.SerializeToArray(_ident.data(), _ident.size());

    int card_len = 0;
    unsigned char* card = idpass_lite_create_card_with_face(
        context, &card_len, _ident.data(), _ident.size());

    // can still create cards without root certs and intermed certs
    EXPECT_TRUE(card != nullptr);
    idpass::IDPassCards fullcard;
    EXPECT_TRUE(fullcard.ParseFromArray(card, card_len));
    
    // rough check that card is created by checking surname field
    ASSERT_TRUE(fullcard.publiccard().details().surname().compare("Pacquiao")
                == 0);

    // and the created card shall have no certificate content
    ASSERT_TRUE(fullcard.certificates_size() == 0);
}

TEST_F(TestCases, idpass_lite_verify_certificate)
{
    CCertificate cert0;
    CCertificate cert1;
    
    cert1.setPublicKey(m_ver, 32); // very important
    cert0.Sign(cert1);
    m_rootCert1->Sign(cert1);

    api::Certificates intermedcerts;
    intermedcerts.add_cert()->CopyFrom(cert0.getValue());
    intermedcerts.add_cert()->CopyFrom(cert1.getValue());

    std::vector<unsigned char> buf(intermedcerts.ByteSizeLong());
    intermedcerts.SerializeToArray(buf.data(), buf.size());

    ASSERT_TRUE(0 == idpass_lite_add_certificates(ctx, buf.data(), buf.size()));

    std::vector<unsigned char> _ident(m_ident.ByteSizeLong());
    m_ident.SerializeToArray(_ident.data(), _ident.size());
    
    int cards_len = 0;
    unsigned char* cards
        = idpass_lite_create_card_with_face(ctx, &cards_len, _ident.data(), _ident.size());

    ASSERT_TRUE(cards != nullptr);
    ASSERT_EQ(idpass_lite_verify_certificate(ctx, cards, cards_len), 2); // 2 certs

    idpass::IDPassCards fullcard;
    ASSERT_TRUE(fullcard.ParseFromArray(cards, cards_len));

    std::vector<idpass::Certificate> cardcerts(fullcard.certificates().begin(),
                                               fullcard.certificates().end());

    ASSERT_TRUE(std::memcmp(cardcerts[0].pubkey().data(), cert0.m_pk.data(), 32) == 0);
    ASSERT_TRUE(std::memcmp(cardcerts[1].pubkey().data(), cert1.m_pk.data(), 32) == 0);
}

TEST_F(TestCases, idpass_lite_init_test)
{
    unsigned char enc[32];
    unsigned char sig[64];
    unsigned char ver[32];

    idpass_lite_generate_secret_signature_keypair(ver, 32, sig, 64);
    idpass_lite_generate_encryption_key(enc, 32);

    void* context = nullptr;

    api::KeySet cryptoKeys;
    api::Certificates rootCerts;

    std::vector<unsigned char> cryptokeys_buf;
    std::vector<unsigned char> rootcerts_buf;

    context = idpass_lite_init(cryptokeys_buf.data(),
                               cryptokeys_buf.size(),
                               rootcerts_buf.data(),
                               rootcerts_buf.size());

    ASSERT_TRUE(context == nullptr);

    cryptoKeys.set_encryptionkey(enc, 32);
    cryptoKeys.set_signaturekey(sig, 64);
    api::byteArray* verkey = cryptoKeys.add_verificationkeys();
    verkey->set_typ(api::byteArray_Typ_ED25519PUBKEY);
    verkey->set_val(ver, 32);

    cryptokeys_buf.resize(cryptoKeys.ByteSizeLong());
    cryptoKeys.SerializeToArray(cryptokeys_buf.data(), cryptokeys_buf.size());

    context = idpass_lite_init(cryptokeys_buf.data(),
                               cryptokeys_buf.size(),
                               rootcerts_buf.data(),
                               rootcerts_buf.size());

    ASSERT_TRUE(context != nullptr); // make rootcerts optional

    CCertificate rootCA;

    idpass::Certificate* pcer = rootCerts.add_cert();
    pcer->CopyFrom(rootCA.getValue());
    rootcerts_buf.resize(rootCerts.ByteSizeLong());
    rootCerts.SerializeToArray(rootcerts_buf.data(), rootcerts_buf.size());

    context = idpass_lite_init(cryptokeys_buf.data(),
                               cryptokeys_buf.size(),
                               rootcerts_buf.data(),
                               rootcerts_buf.size());

    ASSERT_TRUE(context != nullptr);
}

TEST_F(TestCases, idpass_lite_create_card_with_face_test)
{
    api::Ident ident;

    std::vector<unsigned char> ident_buf(ident.ByteSizeLong());
    ident.SerializeToArray(ident_buf.data(), ident_buf.size());

    int buf_len = 0;
    unsigned char* buf = idpass_lite_create_card_with_face(
        ctx, &buf_len, ident_buf.data(), ident_buf.size());

    ASSERT_TRUE(buf == nullptr); // because ident has no photo

    std::string filename = std::string(datapath) + "manny1.bmp";
    std::ifstream photofile(filename, std::ios::binary);
    std::vector<char> photo(std::istreambuf_iterator<char>{photofile}, {});

    std::uint64_t vizflags = DETAIL_PLACEOFBIRTH | DETAIL_GIVENNAME;
    unsigned char ioctlcmd[9];
    ioctlcmd[0] = IOCTL_SET_ACL;
    std::memcpy(&ioctlcmd[1], &vizflags, 8);

    idpass_lite_ioctl(ctx, nullptr, ioctlcmd, sizeof ioctlcmd);

    idpass::Pair* kv = m_ident.add_pubextra();
    kv->set_key("gender");
    kv->set_value("male");

    ident_buf.resize(m_ident.ByteSizeLong());
    m_ident.SerializeToArray(ident_buf.data(), ident_buf.size());

    buf = idpass_lite_create_card_with_face(
        ctx, &buf_len, ident_buf.data(), ident_buf.size());

    ASSERT_TRUE(buf != nullptr);

    idpass::IDPassCards cards;
    ASSERT_TRUE(cards.ParseFromArray(buf, buf_len));

    int details_len = 0;
    unsigned char* details = idpass_lite_verify_card_with_face(
        ctx, &details_len, buf, buf_len, photo.data(), photo.size());

    ASSERT_TRUE(details != nullptr);
}

TEST_F(TestCases, generate_secretsignature_key)
{
    unsigned char sig[crypto_sign_SECRETKEYBYTES]; // 64
    unsigned char pk[crypto_sign_PUBLICKEYBYTES]; // 32
    unsigned char sig2[63];
    int status;

    status = idpass_lite_generate_secret_signature_keypair(pk, 32, sig, sizeof sig);
    ASSERT_TRUE(status == 0);
    status = idpass_lite_generate_secret_signature_keypair(pk, 32, sig2, sizeof sig2);
    ASSERT_TRUE(status != 0);
}

TEST_F(TestCases, generate_encryption_key)
{
    unsigned char enc[crypto_aead_chacha20poly1305_IETF_KEYBYTES]; // 32
    unsigned char enc2[31];
    int status;

    status = idpass_lite_generate_encryption_key(enc, sizeof enc);
    ASSERT_TRUE(status == 0);
    status = idpass_lite_generate_encryption_key(enc2, sizeof enc2);
    ASSERT_TRUE(status != 0);
}

TEST_F(TestCases, chain_of_trust_test)
{
    auto verify_chain = [this](std::vector<CCertificate>& chain, bool expected) {
        int status;
        api::Certificates chaincerts;
        for (auto& c : chain) {
            idpass::Certificate* pCer = chaincerts.add_cert();
            pCer->CopyFrom(c.getValue());
        }
        std::vector<unsigned char> buf(chaincerts.ByteSizeLong());
        chaincerts.SerializeToArray(buf.data(), buf.size());
        status = idpass_lite_add_certificates(ctx, buf.data(), buf.size());
        return status == expected ? 0 : 1;
    };

    unsigned char secret_sig_key[64];
    unsigned char pk[32];
    idpass_lite_generate_secret_signature_keypair(pk, 32, secret_sig_key, 64);

    CCertificate cert2_rootca;
    CCertificate cert7_cert2(secret_sig_key, 64);

    m_rootCert1->Sign(cert2_rootca);
    cert2_rootca.Sign(cert7_cert2);

    std::vector<CCertificate> chain2_valid{cert2_rootca, cert7_cert2};
    ASSERT_TRUE(verify_chain(chain2_valid, true));

    CCertificate c01_c03; // self-signed during creation
    CCertificate c02_c01;
    CCertificate c03_c02;
    c01_c03.Sign(c02_c01);
    c02_c01.Sign(c03_c02);
    c03_c02.Sign(c01_c03); // make it circular

    std::vector<CCertificate> chain_invalid_circular{ c01_c03, c02_c01, c03_c02 };
    ASSERT_TRUE(verify_chain(chain_invalid_circular, false));

    CCertificate gamma;
    CCertificate cert8_gamma;
    gamma.Sign(cert8_gamma);
 
    std::vector<CCertificate> chain12_invalid{gamma, cert8_gamma};
    ASSERT_TRUE(verify_chain(chain12_invalid, false));

    m_rootCert1->Sign(gamma);
    std::vector<CCertificate> chain_valid{gamma, cert8_gamma};
    ASSERT_TRUE(verify_chain(chain_valid, true));

    CCertificate cert001(secret_sig_key, 64);
    m_rootCert1->Sign(cert001);
    std::vector<CCertificate> chain_1{cert001};
    ASSERT_TRUE(verify_chain(chain_1, true));

    CCertificate cert002(secret_sig_key, 64);
    std::vector<CCertificate> chain_2{cert002};
    ASSERT_TRUE(verify_chain(chain_2, false));
}

TEST_F(TestCases, create_card_with_certificates)
{
    CCertificate certifi;
    certifi.setPublicKey(m_ver, 32);

    ASSERT_FALSE(certifi.isSelfSigned());
    ASSERT_FALSE(certifi.hasValidSignature());

    m_rootCert1->Sign(certifi);

    ASSERT_TRUE(certifi.hasValidSignature());
    ASSERT_FALSE(certifi.isSelfSigned());

    api::Certificates intermedCerts;
    idpass::Certificate* pCer = intermedCerts.add_cert();
    pCer->CopyFrom(certifi.getValue()); 

    std::vector<unsigned char> buf(intermedCerts.ByteSizeLong());
    intermedCerts.SerializeToArray(buf.data(), buf.size());

    ASSERT_TRUE(idpass_lite_add_certificates(ctx, buf.data(), buf.size()) == 0);

    // transfer givenname, placeofbirth to public region
    // thus, these fields will no longer be in the private region
    // this is to avoid redundancy 
    std::uint64_t vizflags = DETAIL_PLACEOFBIRTH | DETAIL_GIVENNAME; 
    unsigned char ioctlcmd[9];
    ioctlcmd[0] = IOCTL_SET_ACL; 
    std::memcpy(&ioctlcmd[1], &vizflags, 8);
    idpass_lite_ioctl(ctx, nullptr, ioctlcmd, sizeof ioctlcmd);

    int ecard_len;
    unsigned char* ecard;

    std::vector<unsigned char> ident(m_ident.ByteSizeLong());
    m_ident.SerializeToArray(ident.data(), ident.size());

    ecard = idpass_lite_create_card_with_face(
        ctx, &ecard_len, ident.data(), ident.size());

    ASSERT_TRUE(ecard != nullptr);

    idpass::IDPassCards cards;
    ASSERT_TRUE(cards.ParseFromArray(ecard, ecard_len));

    bool found = false;
    idpass::Certificate certi;
    for (auto& c : cards.certificates()) {
        if (0 == std::memcmp(c.pubkey().data(), 
            certifi.m_pk.data(), 32)) 
        {
            found = true; 
        }
    }

    ASSERT_TRUE(found);

    int details_len = 0;
    unsigned char* details = idpass_lite_verify_card_with_pin(
        ctx, &details_len, ecard, ecard_len, "12345");

    ASSERT_TRUE(details != nullptr);
}

#if 0
TEST_F(DISABLED_TestCases, check_qrcode_md5sum)
{
    auto savetobitmap = [](int qrcode_size,
                           unsigned char* pixelbits,
                           const char* outfile) {
        auto TestBit = [](unsigned char A[], int k) {
            return ((A[k / 8] & (1 << (k % 8))) != 0);
        };

        int width = qrcode_size;
        int height = qrcode_size;

        int size = width * height * 4;
        char header[54] = {0};
        strcpy(header, "BM");
        memset(&header[2], (int)(54 + size), 1);
        memset(&header[10], (int)54, 1); // always 54
        memset(&header[14], (int)40, 1); // always 40
        memset(&header[18], (int)width, 1);
        memset(&header[22], (int)height, 1);
        memset(&header[26], (short)1, 1);
        memset(&header[28], (short)32, 1); // 32bit
        memset(&header[34], (int)size, 1); // pixel size

        unsigned char* pixelbytes = new unsigned char[width * height * 4];
        std::memset(pixelbytes, 0x00, width * height * 4);
        int q = 0;

        for (uint8_t y = 0; y < width; y++) {
            // Each horizontal module
            for (uint8_t x = 0; x < height; x++) {
                int p = (y * height + x) * 4;

                if (TestBit(pixelbits, q++)) {
                    pixelbytes[p + 0] = 255;
                    pixelbytes[p + 1] = 0;
                    pixelbytes[p + 2] = 0;
                } else {
                    pixelbytes[p + 0] = 255;
                    pixelbytes[p + 1] = 255;
                    pixelbytes[p + 2] = 255;
                }
            }
        }

        FILE* fout = fopen(outfile, "wb");
        fwrite(header, 1, 54, fout);
        fwrite(pixelbytes, 1, size, fout);

        delete[] pixelbytes;
        fclose(fout);
    };

    int card_len;
    unsigned char* card;

    std::vector<unsigned char> identbuf(m_ident.ByteSizeLong());
    m_ident.SerializeToArray(identbuf.data(), identbuf.size());

    card = idpass_lite_create_card_with_face(
        ctx, &card_len, identbuf.data(), identbuf.size());

    ASSERT_TRUE(card != nullptr);
    idpass::IDPassCards cards;
    ASSERT_TRUE(cards.ParseFromArray(card, card_len));

    int qrsize = 0;
    unsigned char* pixel = idpass_lite_qrpixel(
        ctx,
        card,
        card_len,
        &qrsize);

    ASSERT_TRUE(pixel != nullptr);

    FILE *fp = fopen("qrcode.dat", "wb");
    int nwritten = 0;
    nwritten = fwrite(card , 1, card_len, fp);
    while (nwritten < card_len) {
        nwritten += fwrite(card , 1, card_len + nwritten, fp);
    }
    fclose(fp);

    savetobitmap(qrsize, pixel, "qrcode.bmp");
    idpass_lite_saveToBitmap(ctx, card, card_len, "qr_code.bmp");
}
#endif

TEST_F(TestCases, createcard_manny_verify_as_brad)
{
    std::string inputfile = std::string(datapath) + "manny1.bmp";
    std::ifstream f1(inputfile, std::ios::binary);
    std::vector<char> img1(std::istreambuf_iterator<char>{f1}, {});

    std::string inputfile2 = std::string(datapath) + "brad.jpg";
    std::ifstream f2(inputfile2, std::ios::binary);
    std::vector<char> img3(std::istreambuf_iterator<char>{f2}, {});

    int ecard_len = 0;
    unsigned char* ecard;

    std::vector<unsigned char> identbuf(m_ident.ByteSizeLong());
    m_ident.SerializeToArray(identbuf.data(), identbuf.size());

    ecard = idpass_lite_create_card_with_face(
        ctx, &ecard_len, identbuf.data(), identbuf.size());

    ASSERT_TRUE(ecard != nullptr);

    int details_len;
    unsigned char* details = idpass_lite_verify_card_with_face(
        ctx, 
        &details_len, 
        ecard, 
        ecard_len, 
        img3.data(), 
        img3.size()
        );

    ASSERT_TRUE(details == nullptr);

    details = idpass_lite_verify_card_with_face(
        ctx, 
        &details_len, 
        ecard, 
        ecard_len, 
        img1.data(), 
        img1.size()
        );

    ASSERT_TRUE(details != nullptr);

    idpass::CardDetails cardDetails;
    cardDetails.ParseFromArray(details, details_len);
    ASSERT_STREQ(cardDetails.surname().c_str(), "Pacquiao");
}

TEST_F(TestCases, cansign_and_verify_with_pin)
{
    std::string inputfile = std::string(datapath) + "manny1.bmp";
    std::ifstream f1(inputfile, std::ios::binary);
    std::vector<char> img1(std::istreambuf_iterator<char>{f1}, {});

    idpass::Dictionary pub_extras;
    idpass::Dictionary priv_extras;

    idpass::Pair *kv = pub_extras.add_pairs();
    kv->set_key("gender");
    kv->set_value("male");

    kv = priv_extras.add_pairs();
    kv->set_key("address");
    kv->set_value("16th Elm Street");

    std::vector<unsigned char> pubExtras(pub_extras.ByteSizeLong());
    std::vector<unsigned char> privExtras(priv_extras.ByteSizeLong());

    pub_extras.SerializeToArray(pubExtras.data(), pubExtras.size());
    priv_extras.SerializeToArray(privExtras.data(), privExtras.size());

    int ecard_len = 0;
    unsigned char* ecard;

    std::vector<unsigned char> identbuf(m_ident.ByteSizeLong());
    m_ident.SerializeToArray(identbuf.data(), identbuf.size());

    ecard = idpass_lite_create_card_with_face(
        ctx,
        &ecard_len,
        identbuf.data(), identbuf.size());

    ASSERT_TRUE(ecard != nullptr);

    idpass::IDPassCards cards;
    ASSERT_TRUE(cards.ParseFromArray(ecard, ecard_len));

    const char* data = "this is a test message";

    unsigned char signature[64];
    ASSERT_TRUE(0 == idpass_lite_sign_with_card(
        ctx,
        signature,
        64,
		ecard,
		ecard_len,
        (unsigned char*)data,
        std::strlen(data)));

    int card_len;
    unsigned char* card = idpass_lite_verify_card_with_pin(
        ctx, 
        &card_len, 
		ecard,
		ecard_len,
        "12345");

    ASSERT_TRUE(card != nullptr);

    idpass::CardDetails cardDetails;
    cardDetails.ParseFromArray(card, card_len);
    //std::cout << cardDetails.surname() << ", " << cardDetails.givenname() << std::endl;

    ASSERT_STREQ(cardDetails.surname().c_str(), "Pacquiao");
    ASSERT_STREQ(cardDetails.givenname().c_str(), "Manny");
}

TEST_F(TestCases, create_card_verify_with_face)
{
    std::string inputfile = std::string(datapath) + "manny1.bmp";
    std::ifstream f1(inputfile, std::ios::binary);
    std::vector<char> img1(std::istreambuf_iterator<char>{f1}, {});

    std::string inputfile2 = std::string(datapath) + "manny2.bmp";
    std::ifstream f2(inputfile2, std::ios::binary);
    std::vector<char> img2(std::istreambuf_iterator<char>{f2}, {});

    std::string inputfile3 = std::string(datapath) + "brad.jpg";
    std::ifstream f3(inputfile3, std::ios::binary);
    std::vector<char> img3(std::istreambuf_iterator<char>{f3}, {});

    int ecard_len;
    unsigned char* ecard;

    std::vector<unsigned char> ident(m_ident.ByteSizeLong());
    m_ident.SerializeToArray(ident.data(), ident.size());

    ecard = idpass_lite_create_card_with_face(
        ctx, &ecard_len, ident.data(), ident.size());

    ASSERT_TRUE(ecard != nullptr);

    idpass::IDPassCards cards;
    cards.ParseFromArray(ecard, ecard_len);
	unsigned char* e_card =(unsigned char*)cards.encryptedcard().c_str();
	int e_card_len = cards.encryptedcard().size();

    int details_len;
    unsigned char* details = idpass_lite_verify_card_with_face(
        ctx, &details_len, 
		ecard,
		ecard_len,
		img3.data(), img3.size());

    ASSERT_TRUE(nullptr == details); // different person's face should not verify

    details = idpass_lite_verify_card_with_face(
        ctx, &details_len, 
		ecard, 
		ecard_len, 
		img2.data(), img2.size());

    ASSERT_TRUE(nullptr != details); // same person's face should verify

    idpass::CardDetails cardDetails;
    cardDetails.ParseFromArray(details, details_len);
    //std::cout << cardDetails.surname() << ", " << cardDetails.givenname() << std::endl;

	// Once verified, the details field should match
    ASSERT_STREQ(cardDetails.surname().c_str(), "Pacquiao");
    ASSERT_STREQ(cardDetails.givenname().c_str(), "Manny"); 
}

TEST_F(TestCases, threading_multiple_instance_test)
{
    // Multiple different instances of idpass_lite_init contexts
    auto multiple_instance_test = [this]()
    {
        unsigned char enc[crypto_aead_chacha20poly1305_IETF_KEYBYTES];
        unsigned char sig_skpk[crypto_sign_SECRETKEYBYTES];
        unsigned char verif_pk[crypto_sign_PUBLICKEYBYTES];

        idpass_lite_generate_encryption_key(enc, 32);
        idpass_lite_generate_secret_signature_keypair(verif_pk, 32, sig_skpk, 64);

        api::KeySet ks;
        ks.set_encryptionkey(enc, 32);
        ks.set_signaturekey(sig_skpk, 64);
        api::byteArray* verkey = ks.add_verificationkeys();
        verkey->set_typ(api::byteArray_Typ_ED25519PUBKEY);
        verkey->set_val(verif_pk, 32);

        CCertificate rootCert1;
        CCertificate rootCert2;

        api::Certificates rootCertificates;
        idpass::Certificate* content1 = rootCertificates.add_cert();
        idpass::Certificate* content2 = rootCertificates.add_cert();
        content1->CopyFrom(rootCert1.getValue());
        content2->CopyFrom(rootCert1.getValue());

        CCertificate intermedCert1;
        CCertificate intermedCert2;
        intermedCert2.setPublicKey(verif_pk, 32);
        intermedCert1.Sign(intermedCert2);
        rootCert1.Sign(intermedCert1);

        std::vector<unsigned char> rootcertsbuf(
            rootCertificates.ByteSizeLong());
        rootCertificates.SerializeToArray(rootcertsbuf.data(),
                                          rootcertsbuf.size());
        std::vector<unsigned char> keysetbuf(ks.ByteSizeLong());
        ks.SerializeToArray(keysetbuf.data(), keysetbuf.size());

        void* context = idpass_lite_init(keysetbuf.data(),
                                         keysetbuf.size(),
                                         rootcertsbuf.data(),
                                         rootcertsbuf.size());
        ASSERT_TRUE(context != nullptr);

        std::string inputfile = std::string(datapath) + "manny1.bmp";
        std::ifstream f1(inputfile, std::ios::binary);
        ASSERT_TRUE(f1.is_open());

        std::vector<char> photo(std::istreambuf_iterator<char>{f1}, {});
        int card_len = 0;
        unsigned char* card;

        std::vector<unsigned char> identbuf(m_ident.ByteSizeLong());
        m_ident.SerializeToArray(identbuf.data(), identbuf.size());

        card = idpass_lite_create_card_with_face(
            context, &card_len, identbuf.data(), identbuf.size());

        ASSERT_TRUE(card != nullptr);

        idpass::IDPassCards cards;
        ASSERT_TRUE(cards.ParseFromArray(card, card_len));

        int buf_len;
        unsigned char* buf = idpass_lite_verify_card_with_face(
            context, &buf_len, card, card_len, photo.data(), photo.size());

        ASSERT_TRUE(buf != nullptr);
        
        idpass::CardDetails userDetails;
        ASSERT_TRUE(userDetails.ParseFromArray(buf, buf_len));
    };

    const int N = 3;
    std::thread* T[N];

    for (int i = 0; i < N; i++) {
        T[i] = new std::thread(multiple_instance_test); 
    }

    std::for_each(T, T + N, [](std::thread* t) { 
        t->join(); 
        delete t;
    });
}

TEST_F(TestCases, threading_single_instance_test)
{
    // A single instance of idpass_api_init context
    // called in multiple threads
    auto single_instance_test = [this](void* ctx)
    {
        std::string inputfile = std::string(datapath) + "manny1.bmp";
        std::ifstream f1(inputfile, std::ios::binary);
        ASSERT_TRUE(f1.is_open());

        std::vector<char> photo(std::istreambuf_iterator<char>{f1}, {});
        std::uint64_t vizflags = DETAIL_PLACEOFBIRTH | DETAIL_GIVENNAME;// make givenname, placeofbirth visible
        unsigned char ioctlcmd[9];
        ioctlcmd[0] = IOCTL_SET_ACL;
        std::memcpy(&ioctlcmd[1], &vizflags, 8);
        idpass_lite_ioctl(ctx, nullptr, ioctlcmd, sizeof ioctlcmd);

        std::vector<unsigned char> identbuf(m_ident.ByteSizeLong());
        m_ident.SerializeToArray(identbuf.data(), identbuf.size());

        int card_len;
        unsigned char* card = idpass_lite_create_card_with_face(
            ctx, &card_len, identbuf.data(), identbuf.size());

        ASSERT_TRUE(card != nullptr);

        idpass::IDPassCards cards;
        ASSERT_TRUE(cards.ParseFromArray(card, card_len));

        ASSERT_TRUE(cards.publiccard().details().surname().empty());
        ASSERT_TRUE(cards.publiccard().details().givenname().compare("Manny")
                    == 0);
        ASSERT_TRUE(cards.publiccard().details().dateofbirth().ByteSizeLong()
                    == 0);
        ASSERT_TRUE(cards.publiccard().details().placeofbirth().compare(
                        "Kibawe, Bukidnon")
                    == 0);

        int buf_len;
        unsigned char* buf = idpass_lite_verify_card_with_face(
            ctx, &buf_len, card, card_len, photo.data(), photo.size());

        ASSERT_TRUE(buf != nullptr);

        idpass::CardDetails details;
        ASSERT_TRUE(details.ParseFromArray(buf, buf_len));
    };

    const int N = 3;
    std::thread* T[N];

    for (int i = 0; i < N; i++) {
        T[i] = new std::thread(single_instance_test, ctx); 
    }
   
    std::for_each(T, T + N, [](std::thread* t) { 
        t->join(); 
        delete t;
    });
}

TEST_F(TestCases, face_template_test)
{
    std::string inputfile1 = std::string(datapath) + "manny1.bmp";
    std::string inputfile2 = std::string(datapath) + "manny2.bmp";

    std::ifstream f1(inputfile1, std::ios::binary);
    std::ifstream f2(inputfile2, std::ios::binary);

    std::vector<char> photo1(std::istreambuf_iterator<char>{f1}, {}); 
    std::vector<char> photo2(std::istreambuf_iterator<char>{f2}, {}); 

    float result_half = -10.0f;
    float result_full = -10.0f;
    int status;

    unsigned char photo1_template_full[128 * 4];
    unsigned char photo2_template_full[128 * 4];

    unsigned char photo1_template_half[64 * 2];
    unsigned char photo2_template_half[64 * 2];

    idpass_lite_face128dbuf( ctx, photo1.data(), photo1.size(), photo1_template_full);
    idpass_lite_face128dbuf( ctx, photo2.data(), photo2.size(), photo2_template_full);

    idpass_lite_face64dbuf( ctx, photo1.data(), photo1.size(), photo1_template_half);
    idpass_lite_face64dbuf( ctx, photo2.data(), photo2.size(), photo2_template_half);
    
    status = idpass_lite_compare_face_template(
                                     photo1_template_full,
                                     sizeof photo1_template_full,
                                     photo2_template_full,
                                     sizeof photo2_template_full,
                                     &result_full); // 0.499922544

    ASSERT_TRUE(status == 0); 

    status = idpass_lite_compare_face_template(
                                     photo1_template_half,
                                     sizeof photo1_template_half,
                                     photo2_template_half,
                                     sizeof photo2_template_half,
                                     &result_half); // 0.394599169
    ASSERT_TRUE(status == 0);                                     
}

TEST_F(TestCases, idpass_lite_verify_with_card_test)
{
    std::vector<unsigned char> _ident(m_ident.ByteSizeLong());
    m_ident.SerializeToArray(_ident.data(), _ident.size());

    const char* msg = "attack at dawn!";
    int msg_len = strlen(msg);
    unsigned char signature[64];
    int signature_len = 64;
    unsigned char pubkey[32];
    int pubkey_len = 32;

    int card_len = 0;
    unsigned char* card = idpass_lite_create_card_with_face(ctx, 
        &card_len, _ident.data(), _ident.size());

    ASSERT_TRUE(card != nullptr);

    ///////////// get user's unique ed25519 key ////////////
    idpass::IDPassCards idpassCards;
    ASSERT_TRUE(idpassCards.ParseFromArray(card, card_len));
    idpass::SignedIDPassCard signedidpasscard; // watch for this extra envelope!!

    int decrypted_card_len = idpassCards.encryptedcard().size();
    std::vector<unsigned char> decrypted_card(
        idpassCards.encryptedcard().begin(), idpassCards.encryptedcard().end());

    /////////////////////////////////////////////////////////////////////
    // the input encrypted buffer is also the output for decrypted buffer
    // but must pass and use the decrypted_card_len pointer, as the encrypted
    // len includes the nonce, but the decrypted has no more nonce.
    int status = idpass_lite_card_decrypt(
        ctx, decrypted_card.data(), &decrypted_card_len, m_enc, 32);

    ASSERT_EQ(status, 0);
    ASSERT_TRUE(signedidpasscard.ParseFromArray(decrypted_card.data(), decrypted_card_len));
    idpass::IDPassCard idpassCard = signedidpasscard.card();
    unsigned char card_skpk[64];
    std::memcpy(card_skpk, idpassCard.encryptionkey().data(), 64);
    std::memcpy(pubkey, card_skpk + 32, 32);
    //////////////////////////////////////////////////////////////
    ASSERT_TRUE(
        0
        == idpass_lite_sign_with_card(
            ctx, signature, 64, card, card_len, (unsigned char*)msg, msg_len));

    status = idpass_lite_verify_with_card(
        ctx, (unsigned char*)msg, msg_len, signature, signature_len, pubkey, pubkey_len);

    ASSERT_EQ(status, 0);
}

TEST_F(TestCases, test_card_encrypt_decrypt)
{
    const char* msg = "attack at dawn!";

    std::vector<unsigned char> _ident(m_ident.ByteSizeLong());
    m_ident.SerializeToArray(_ident.data(), _ident.size());

    int card_len = 0;
    unsigned char* card = idpass_lite_create_card_with_face(ctx, 
        &card_len, _ident.data(), _ident.size());

    ASSERT_TRUE(card != nullptr);

    ///////////// get user's unique ed25519 key ////////////
    idpass::IDPassCards idpassCards;
    ASSERT_TRUE(idpassCards.ParseFromArray(card, card_len));
    idpass::SignedIDPassCard signedidpasscard; // watch for this extra envelope!!

    int decrypted_card_len = idpassCards.encryptedcard().size();
    std::vector<unsigned char> decrypted_card(
        idpassCards.encryptedcard().begin(), idpassCards.encryptedcard().end());

    /////////////////////////////////////////////////////////////////////
    // the input encrypted buffer is also the output for decrypted buffer
    // but must pass and use the decrypted_card_len pointer, as the encrypted
    // len includes the nonce, but the decrypted has no more nonce.
    int status = idpass_lite_card_decrypt(
        ctx, decrypted_card.data(), &decrypted_card_len, m_enc, 32);

    ASSERT_EQ(status, 0);
    ASSERT_TRUE(signedidpasscard.ParseFromArray(decrypted_card.data(), decrypted_card_len));
    idpass::IDPassCard idpassCard = signedidpasscard.card();
    unsigned char card_skpk[64];
    std::memcpy(card_skpk, idpassCard.encryptionkey().data(), 64);
    //////////////////////////////////////////////////////////////

    int encrypted_len = 0;
    unsigned char* encrypted
        = idpass_lite_encrypt_with_card(ctx,
                                        &encrypted_len,
                                        card,
                                        card_len,
                                        (unsigned char*)(msg),
                                        strlen(msg));

    ASSERT_TRUE(encrypted != nullptr && encrypted_len > 0);

    int decrypted_len;
    unsigned char* decrypted = idpass_lite_decrypt_with_card(
        ctx, &decrypted_len, card, card_len, 
        (unsigned char*)encrypted, 
        encrypted_len);

    ASSERT_TRUE(decrypted != nullptr && decrypted_len > 1);
    ASSERT_TRUE(std::memcmp(msg, decrypted, strlen(msg)) == 0);
}

TEST_F(TestCases, uio_test)
{
    unsigned char* buf = idpass_lite_uio(ctx, 0);
    int len;
    std::memcpy(&len, buf, sizeof(int));
    api::Ident ident;
    ASSERT_TRUE(ident.ParseFromArray(buf + sizeof(int), len));
    ASSERT_TRUE(0 == ident.surname().compare("Doe"));
    ASSERT_TRUE(0 == ident.givenname().compare("John"));
}

TEST_F(TestCases, idpass_ioctl_test)
{
    // IOCTL_SET_FACEDIFF,IOCTL_GET_FACEDIFF
    float fdiff = 0.43;
    unsigned char ioctlcmd[5];
    ioctlcmd[0] = IOCTL_SET_FACEDIFF;
    std::memcpy(&ioctlcmd[1], &fdiff, 4);
    idpass_lite_ioctl(ctx, nullptr, ioctlcmd, sizeof ioctlcmd);
    std::memset(ioctlcmd, 0x00, 5);
    ioctlcmd[0] = IOCTL_GET_FACEDIFF;
    float fval;
    idpass_lite_ioctl(ctx, nullptr, ioctlcmd, sizeof ioctlcmd);
    std::memcpy(&fval, &ioctlcmd[1], 4);
    ASSERT_EQ(fdiff, fval);

    // IOCTL_SET_FDIM, IOCTL_GET_FDIM
    std::memset(ioctlcmd, 0x00, 5);
    ioctlcmd[0] = IOCTL_SET_FDIM;
    ioctlcmd[1] = 0x00;
    idpass_lite_ioctl(ctx, nullptr, ioctlcmd, sizeof ioctlcmd);
    std::memset(ioctlcmd, 0x00, 5);
    ioctlcmd[0] = IOCTL_GET_FDIM;
    idpass_lite_ioctl(ctx, nullptr, ioctlcmd, sizeof ioctlcmd);
    ASSERT_EQ(ioctlcmd[1], 0x00);
    std::memset(ioctlcmd, 0x00, 5);
    ioctlcmd[0] = IOCTL_SET_FDIM;
    ioctlcmd[1] = 0x01;
    idpass_lite_ioctl(ctx, nullptr, ioctlcmd, sizeof ioctlcmd);
    std::memset(ioctlcmd, 0x00, 5);
    ioctlcmd[0] = IOCTL_GET_FDIM;
    idpass_lite_ioctl(ctx, nullptr, ioctlcmd, sizeof ioctlcmd);
    ASSERT_EQ(ioctlcmd[1], 0x01);

    // IOCTL_SET_ECC
    std::memset(ioctlcmd, 0x00, 5);
    ioctlcmd[0] = IOCTL_SET_ECC;
    ioctlcmd[1] = ECC_LOW;
    idpass_lite_ioctl(ctx, nullptr, ioctlcmd, sizeof ioctlcmd);
    std::memset(ioctlcmd, 0x00, 5);
    ioctlcmd[0] = IOCTL_SET_ECC;
    ioctlcmd[1] = ECC_MEDIUM;
    idpass_lite_ioctl(ctx, nullptr, ioctlcmd, sizeof ioctlcmd);
    std::memset(ioctlcmd, 0x00, 5);
    ioctlcmd[0] = IOCTL_SET_ECC;
    ioctlcmd[1] = ECC_QUARTILE;
    idpass_lite_ioctl(ctx, nullptr, ioctlcmd, sizeof ioctlcmd);
    std::memset(ioctlcmd, 0x00, 5);
    ioctlcmd[0] = IOCTL_SET_ECC;
    ioctlcmd[1] = ECC_HIGH;
    idpass_lite_ioctl(ctx, nullptr, ioctlcmd, sizeof ioctlcmd);
}

TEST_F(TestCases, face_compute_test)
{
    std::string filename = std::string(datapath) + "manny1.bmp";
    std::ifstream photofile(filename, std::ios::binary);
    std::vector<char> photo(std::istreambuf_iterator<char>{photofile}, {});
    float facearray64[64];
    ASSERT_EQ(1, idpass_lite_face64d(ctx, photo.data(), photo.size(), facearray64));
    float facearray128[128];
    ASSERT_EQ(1, idpass_lite_face128d(ctx, photo.data(), photo.size(), facearray128));
}

TEST_F(TestCases, qrcode_test)
{
    int qrsize = 0;
    int buf_len = 0;

    std::vector<unsigned char> _ident(m_ident.ByteSizeLong());
    m_ident.SerializeToArray(_ident.data(), _ident.size());

    int len;
    unsigned char* cards
        = idpass_lite_create_card_with_face(ctx, &len, _ident.data(), _ident.size());

    ASSERT_TRUE(cards != nullptr);
    unsigned char* buf = idpass_lite_qrpixel2(ctx, &buf_len, cards, len, &qrsize);
    ASSERT_TRUE(buf != nullptr);
    idpass_lite_freemem(ctx, buf);
}

TEST_F(TestCases, certificate_revoke_test)
{
    std::string filename = std::string(datapath) + "manny1.bmp";
    std::ifstream photofile(filename, std::ios::binary);
    std::vector<char> photo(std::istreambuf_iterator<char>{photofile}, {});

    std::vector<unsigned char> ident_buf(m_ident.ByteSizeLong());
    m_ident.SerializeToArray(ident_buf.data(), ident_buf.size());

    int n;

    CCertificate child0;
    CCertificate child1(m_sig, 64);
    m_rootCert1->Sign(child0);
    child0.Sign(child1);

    api::Certificates intermediateCertificates;
    idpass::Certificate* c1 = intermediateCertificates.add_cert();
    c1->CopyFrom(child0.getValue());
    idpass::Certificate* c2 = intermediateCertificates.add_cert();
    c2->CopyFrom(child1.getValue());

    std::vector<unsigned char> intermedcerts_buf(intermediateCertificates.ByteSizeLong());

    intermediateCertificates.SerializeToArray(intermedcerts_buf.data(),
                                              intermedcerts_buf.size());

    idpass_lite_add_revoked_key((unsigned char*)child0.m_pk.data(), 32);

    n = idpass_lite_add_certificates(
        ctx, intermedcerts_buf.data(), intermedcerts_buf.size());

    ASSERT_TRUE(n != 0); // cannot add chain since child0 is in revoked list

}

TEST_F(TestCases, compare_face_photo_test)
{
    std::string file1 = std::string(datapath) + "manny1.bmp";
    std::ifstream photofile1(file1, std::ios::binary);
    std::vector<char> photo1(std::istreambuf_iterator<char>{photofile1}, {});

    std::string file2 = std::string(datapath) + "manny1.bmp";
    std::ifstream photofile2(file2, std::ios::binary);
    std::vector<char> photo2(std::istreambuf_iterator<char>{photofile2}, {});

    std::string file3 = std::string(datapath) + "manny2.bmp";
    std::ifstream photofile3(file3, std::ios::binary);
    std::vector<char> photo3(std::istreambuf_iterator<char>{photofile3}, {});

    std::string file4 = std::string(datapath) + "manny4.jpg";
    std::ifstream photofile4(file4, std::ios::binary);
    std::vector<char> photo4(std::istreambuf_iterator<char>{photofile4}, {});

    std::string file5 = std::string(datapath) + "manny5.jpg";
    std::ifstream photofile5(file5, std::ios::binary);
    std::vector<char> photo5(std::istreambuf_iterator<char>{photofile5}, {});

    float fdif;
    int status = idpass_lite_compare_face_photo(ctx, 
        photo1.data(), photo1.size(), 
        photo2.data(), photo2.size(),
        &fdif);

    ASSERT_EQ(fdif, 0.0); // because exactly same photo

    float threshold;
    unsigned char ioctlcmd[5];
    ioctlcmd[0] = IOCTL_GET_FACEDIFF;
    idpass_lite_ioctl(ctx, nullptr, ioctlcmd, sizeof ioctlcmd);
    std::memcpy(&threshold, &ioctlcmd[1], 4);

    idpass_lite_compare_face_photo(ctx, 
        photo1.data(), photo1.size(), 
        photo4.data(), photo4.size(),
        &fdif);

    ASSERT_TRUE(fdif < threshold);

    idpass_lite_compare_face_photo(ctx, 
        photo2.data(), photo2.size(), 
        photo5.data(), photo5.size(),
        &fdif);

    ASSERT_TRUE(fdif < threshold);

    idpass_lite_compare_face_photo(ctx, 
        photo4.data(), photo4.size(), 
        photo5.data(), photo5.size(),
        &fdif);

    ASSERT_TRUE(fdif < threshold);

    idpass_lite_compare_face_photo(ctx, 
        photo1.data(), photo1.size(), 
        photo5.data(), photo5.size(),
        &fdif);

    ASSERT_TRUE(fdif < threshold);

    idpass_lite_compare_face_photo(ctx, 
        photo2.data(), photo2.size(), 
        photo4.data(), photo4.size(),
        &fdif);

    ASSERT_TRUE(fdif < threshold);

    idpass_lite_compare_face_photo(ctx, 
        photo2.data(), photo2.size(), 
        photo5.data(), photo5.size(),
        &fdif);

    ASSERT_TRUE(fdif < threshold);
}

TEST_F(TestCases, bin16_tests)
{
    float ff[128]; // full float

    for (int i = 0; i < 128; i++) {
        ff[i] = -10.0 + static_cast<float>(rand()) / 
            (static_cast<float>(RAND_MAX / (10.0 - (-10.0))));
    }

    unsigned char fbuf[128 * 4];
    bin16::f4_to_f4b(ff, 128, fbuf);

    float hf1[128]; // half float
    bin16::f4b_to_f2(fbuf, 128 * 4, hf1);

    unsigned char f2buf[128 * 2];
    bin16::f4b_to_f2b(fbuf, 128 * 4, f2buf);

    float hf2[128];
    bin16::f2b_to_f2(f2buf, 128 * 2, hf2);

    for (int i = 0; i < 128; i++) {
        ASSERT_EQ(hf1[i], hf2[i]);
    }
}

TEST_F(TestCases, sig_invariance_tests)
{
    unsigned char data[512];

    unsigned char* sig;
    unsigned char sig1[64];
    unsigned char sig2[64];

    unsigned char pk[32];
    unsigned char sk[64];
    crypto_sign_keypair(pk, sk);

    randombytes_buf(data, 512); 

    for (int i = 0; i < 1000; i++) {
        sig = (i % 2 == 0) ? &sig1[0] : &sig2[0];
        int status = crypto_sign_detached(sig, nullptr, data, 512, sk);
        ASSERT_EQ(status, 0);
        if (i > 0) {
            ASSERT_EQ(std::memcmp(sig1, sig2, 64), 0);
        }
    }
}

// enable ALWAYS processor when compiling libidpasslite.so
// for this test to makes sense
// This test is to prove that similar identity details
// should produce exactly the same QR code
#if 0
TEST_F(TestCases, card_invariance_test)
{
    api::KV* privextra = m_ident.add_privextra();
    privextra->set_key("color");
    privextra->set_value("blue");

    std::vector<unsigned char> buf(m_ident.ByteSizeLong());
    m_ident.SerializeToArray(buf.data(), buf.size());

    unsigned char hash1[crypto_generichash_BYTES];
    unsigned char hash2[crypto_generichash_BYTES];
    unsigned char* hash;

    int qrsidelen;
    int qrbuf_len;
    std::vector<unsigned char> qrbuf1;
    std::vector<unsigned char> qrbuf2;
    unsigned char* qrbuf;
    
    int cards_len;
    unsigned char* cards;

    for (int i = 0; i < 1000; i++) {
        cards_len = 0;
        cards = nullptr;

        cards = idpass_lite_create_card_with_face(
            ctx, &cards_len, buf.data(), buf.size());

        ASSERT_TRUE(cards != nullptr && cards_len > 0);
        hash = (i % 2 == 0) ? &hash1[0] : &hash2[0];
        int status = idpass_lite_compute_hash(
            cards, cards_len, hash, crypto_generichash_BYTES);
        ASSERT_EQ(status, 0);

        qrbuf_len = 0;
        qrbuf = nullptr;
        qrbuf = idpass_lite_qrpixel2(ctx, &qrbuf_len, cards, cards_len, &qrsidelen);

        ASSERT_TRUE(qrbuf != nullptr && qrbuf_len > 0);

        if (i % 2 == 0) {
            qrbuf1.resize(qrbuf_len);
            qrbuf1.clear();
            std::memcpy(qrbuf1.data(), qrbuf, qrbuf_len);
        } else {
            qrbuf2.resize(qrbuf_len);
            qrbuf2.clear();
            std::memcpy(qrbuf2.data(), qrbuf, qrbuf_len);
        }

        if (i > 0) {
            ASSERT_EQ(std::memcmp(hash1, hash2, crypto_generichash_BYTES), 0); 
            ASSERT_EQ(std::memcmp(qrbuf1.data(), qrbuf2.data(), qrbuf_len), 0);
        }
    }
}
#endif

TEST_F(TestCases, create_card_no_photo)
{
    api::Ident ident;

    ident.set_surname("Pacquiao");
    ident.set_givenname("Manny");
    ident.set_placeofbirth("Kibawe, Bukidnon");
    ident.set_pin("12345");
    ident.mutable_dateofbirth()->set_year(1978);
    ident.mutable_dateofbirth()->set_month(12);
    ident.mutable_dateofbirth()->set_day(17);

    std::vector<unsigned char> buf(ident.ByteSizeLong());
    ident.SerializeToArray(buf.data(), buf.size());

    int cards_len;
    unsigned char* cards
        = idpass_lite_create_card_with_face(ctx, &cards_len, buf.data(), buf.size());

    ASSERT_TRUE(cards != nullptr); 

    int details_len = 0;
    unsigned char* details = idpass_lite_verify_card_with_pin(
        ctx, &details_len, cards, cards_len, "12345"); // pin to open the card

    ASSERT_TRUE(details != nullptr);

    {
        std::string inputfile = std::string(datapath) + "manny1.bmp";
        std::ifstream f1(inputfile, std::ios::binary);
        std::vector<char> photo(std::istreambuf_iterator<char>{f1}, {});

        int details_len = 0;
        unsigned char* details = idpass_lite_verify_card_with_face(
            ctx, &details_len, cards, cards_len, photo.data(), photo.size());

        // cannot open card
        ASSERT_TRUE(details == nullptr); // because issued ID has no facial biometry
    }
}

TEST_F(TestCases, test_new_protobuf_fields)
{
    int buf_len = 0;
    unsigned char* buf;

    api::Ident o_ident;

    std::string filename = std::string(datapath) + "manny1.bmp";
    std::ifstream photofile(filename, std::ios::binary);
    std::vector<char> photo(std::istreambuf_iterator<char>{photofile}, {});

    o_ident.set_surname("Pacquiao");
    o_ident.set_givenname("Manny");
    o_ident.set_placeofbirth("Kibawe, Bukidnon");
    o_ident.set_pin("12345");
    //o_ident.mutable_dateofbirth()->set_year(1978);
    //o_ident.mutable_dateofbirth()->set_month(12);
    //o_ident.mutable_dateofbirth()->set_day(17);
    o_ident.set_photo(photo.data(), photo.size());

    std::string fullname = o_ident.givenname() + " " + o_ident.surname();

    o_ident.set_fullname(fullname);
    o_ident.set_uin("314159");
    o_ident.set_gender(2);
    o_ident.mutable_postaladdress()->set_language_code("en");
    o_ident.mutable_postaladdress()->set_organization("NEWLOGIC");

    std::vector<unsigned char> ident_buf(o_ident.ByteSizeLong());
    ident_buf.resize(o_ident.ByteSizeLong());
    o_ident.SerializeToArray(ident_buf.data(), ident_buf.size());

    buf = idpass_lite_create_card_with_face(
        ctx, &buf_len, ident_buf.data(), ident_buf.size());

    ASSERT_TRUE(buf != nullptr);

    idpass::IDPassCards cards;
    ASSERT_TRUE(cards.ParseFromArray(buf, buf_len));

    int details_len = 0;
    unsigned char* details = idpass_lite_verify_card_with_face(
        ctx, &details_len, buf, buf_len, photo.data(), photo.size());

    ASSERT_TRUE(details != nullptr);

    idpass::CardDetails cardDetails;
    cardDetails.ParseFromArray(details, details_len);
    ASSERT_STREQ(cardDetails.fullname().c_str(),fullname.c_str());
    ASSERT_STREQ(cardDetails.postaladdress().organization().c_str(),"NEWLOGIC");
    ASSERT_STREQ(cardDetails.postaladdress().language_code().c_str(),"en");
    ASSERT_STREQ(cardDetails.uin().c_str(),"314159");
    ASSERT_EQ(cardDetails.gender(),2);
}

TEST_F(TestCases, test_merge_CardDetails)
{
    idpass::CardDetails d1;

    std::map<std::string, std::string> d1Extras = {
        {"Ethnicity","Caucasian"}, 
        {"Email","johndoe@email.com"}
    };

    d1.set_fullname("MR. JOHN DOE");
    d1.set_uin("14443");
    for (auto& x : d1Extras) {
        idpass::Pair* extra = d1.add_extra();
        extra->set_key(x.first);
        extra->set_value(x.second);
    }

    idpass::CardDetails d2;

    d2.set_givenname("John");
    d2.set_surname("Doe");

    std::vector<unsigned char> d1buf(d1.ByteSizeLong());
    d1.SerializeToArray(d1buf.data(), d1buf.size());

    std::vector<unsigned char> d2buf(d2.ByteSizeLong());
    d2.SerializeToArray(d2buf.data(), d2buf.size());

    int buf_len = 0;
    unsigned char* buf = idpass_lite_merge_CardDetails(
        d1buf.data(), d1buf.size(), d2buf.data(), d2buf.size(), &buf_len);

    idpass::CardDetails merged;
    bool flag;

    if (buf != nullptr) {
        flag = merged.ParseFromArray(buf, buf_len);
        idpass_lite_freemem(nullptr, buf);
    }

    ASSERT_EQ(merged.givenname(), d2.givenname());
    ASSERT_EQ(merged.surname(), d2.surname());
    ASSERT_EQ(merged.fullname(), d1.fullname());
    ASSERT_EQ(merged.uin(), d1.uin());
    ASSERT_EQ(merged.extra_size(), 2);
    for (auto& x : merged.extra()) {
        std::string k = x.key();         
        std::string v = x.value();
        ASSERT_TRUE(d1Extras.find(k) != d1Extras.end());
        ASSERT_EQ(d1Extras[k], v);
    }
}

TEST_F(TestCases, test_verify_card_with_face_template)
{
    std::string inputfile = std::string(datapath) + "manny1.bmp";
    std::ifstream f1(inputfile, std::ios::binary);
    std::vector<char> photo(std::istreambuf_iterator<char>{f1}, {});

    std::uint64_t vizflags = DETAIL_SURNAME | DETAIL_PLACEOFBIRTH;
    unsigned char ioctlcmd[9];
    ioctlcmd[0] = IOCTL_SET_ACL;
    std::memcpy(&ioctlcmd[1], &vizflags, 8);

    idpass_lite_ioctl(ctx, nullptr, ioctlcmd, sizeof ioctlcmd);

    idpass::Pair* privextra = m_ident.add_privextra();
    privextra->set_key("color");
    privextra->set_value("blue");

    std::vector<unsigned char> buf(m_ident.ByteSizeLong());
    m_ident.SerializeToArray(buf.data(), buf.size());
    
    int cards_len;
    unsigned char* cards
        = idpass_lite_create_card_with_face(ctx, &cards_len, buf.data(), buf.size());

    ASSERT_TRUE(cards != nullptr); 

    // Compute the 128D face descriptor
    float faceArray[128];
    int result = idpass_lite_face128d(ctx, photo.data(), photo.size(), faceArray);
    ASSERT_NE(result, 0); // Check that the function succeeded

    int outlen;
    unsigned char* details = idpass_lite_verify_card_with_face_template(ctx, &outlen, cards, cards_len, reinterpret_cast<unsigned char*>(faceArray), sizeof(faceArray));

    ASSERT_NE(details, nullptr);
}

TEST_F(TestCases, test_verify_card_with_face_template_with_different_photo)
{
    std::string inputfile = std::string(datapath) + "brad.jpg";
    std::ifstream f1(inputfile, std::ios::binary);
    std::vector<char> photo(std::istreambuf_iterator<char>{f1}, {});

    std::uint64_t vizflags = DETAIL_SURNAME | DETAIL_PLACEOFBIRTH;
    unsigned char ioctlcmd[9];
    ioctlcmd[0] = IOCTL_SET_ACL;
    std::memcpy(&ioctlcmd[1], &vizflags, 8);

    idpass_lite_ioctl(ctx, nullptr, ioctlcmd, sizeof ioctlcmd);

    idpass::Pair* privextra = m_ident.add_privextra();
    privextra->set_key("color");
    privextra->set_value("blue");

    std::vector<unsigned char> buf(m_ident.ByteSizeLong());
    m_ident.SerializeToArray(buf.data(), buf.size());
    
    int cards_len;
    unsigned char* cards
        = idpass_lite_create_card_with_face(ctx, &cards_len, buf.data(), buf.size());

    ASSERT_TRUE(cards != nullptr); 

    // Compute the 128D face descriptor
    float faceArray[128];
    int result = idpass_lite_face128d(ctx, photo.data(), photo.size(), faceArray);
    ASSERT_NE(result, 0); // Check that the function succeeded

    int outlen;
    unsigned char* details = idpass_lite_verify_card_with_face_template(ctx, &outlen, cards, cards_len, reinterpret_cast<unsigned char*>(faceArray), sizeof(faceArray));

    ASSERT_EQ(details, nullptr);
}

int main(int argc, char* argv[])
{
    if (argc > 1) {
        datapath = argv[1];
    }

    struct stat statbuf;
    if (stat(datapath, &statbuf) != -1) {
        if (S_ISDIR(statbuf.st_mode)) {
            ::testing::InitGoogleTest(&argc, argv);
            //::testing::GTEST_FLAG(filter) = "TestCases.uio_test";
            //::testing::GTEST_FLAG(filter) = "TestCases.createcard_manny_verify_as_brad";
            //::testing::GTEST_FLAG(filter) = "TestCases.threading_multiple_instance_test";
            //::testing::GTEST_FLAG(filter) = "TestCases.test_card_encrypt_decrypt";
            //::testing::GTEST_FLAG(filter) = "TestCases.idpass_lite_verify_with_card_test";
            //::testing::GTEST_FLAG(filter) = "TestCases.cansign_and_verify_with_pin";
            //::testing::GTEST_FLAG(filter) = "TestCases.face_template_test";
            //::testing::GTEST_FLAG(filter) = "TestCases.idpass_ioctl_test";
            //::testing::GTEST_FLAG(filter) = "TestCases.face_compute_test";
            //::testing::GTEST_FLAG(filter) = "TestCases.qrcode_test";
            //::testing::GTEST_FLAG(filter) = "TestCases.certificate_revoke_test";
            //::testing::GTEST_FLAG(filter) = "TestCases.compare_face_photo_test";
            //::testing::GTEST_FLAG(filter) = "TestCases.check_qrcode_md5sum";
            //::testing::GTEST_FLAG(filter) = "TestCases.create_card_with_certificates_content_tampering";
            //::testing::GTEST_FLAG(filter) = "TestCases.idpass_lite_create_card_with_face_certificates";
            //::testing::GTEST_FLAG(filter) = "TestCases.idpass_lite_verify_certificate";
            //::testing::GTEST_FLAG(filter) = "TestCases.card_integrity_test";
            //::testing::GTEST_FLAG(filter) = "TestCases.sig_invariance_tests";
            //::testing::GTEST_FLAG(filter) = "TestCases.card_invariance_test";
            //::testing::GTEST_FLAG(filter) = "*create_card_no_photo*";

            //testing::GTEST_FLAG(filter) = "*test_new_protobuf_fields*";
            //testing::GTEST_FLAG(filter) = "*test_merge_CardDetails*";
            //testing::GTEST_FLAG(filter) = "*test_verify_card_with_face_template*";
            //testing::GTEST_FLAG(filter) = "*test_verify_card_with_face_template_with_different_photo*";
            return RUN_ALL_TESTS();
        }
    }

    std::cout
        << "The data folder must exists relative to the executable's location\n";
    std::cout << "Or specify data path. For example:\n";
    std::cout << "./idpasstests lib/tests/data\n";
    return 0;
}
