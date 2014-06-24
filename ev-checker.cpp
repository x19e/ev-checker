/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>

#include "EVCheckerTrustDomain.h"
#include "Util.h"
#include "nss.h"
#include "hasht.h"
#include "pk11pub.h"
#include "plbase64.h"
#include "plgetopt.h"
#include "prerror.h"
#include "secerr.h"

void
PrintUsage(const char* argv0)
{
  std::cerr << "Usage: " << argv0 << " <-c certificate list file (PEM format)>";
  std::cerr << " <-o dotted EV policy OID> <-d EV policy description>";
  std::cerr << std::endl << std::endl;
  std::cerr << "(the certificate list is expected to have the root first, ";
  std::cerr << "followed by one or more intermediates, followed by the ";
  std::cerr << "end-entity certificate)" << std::endl;
}

inline void
SECITEM_FreeItem_true(SECItem* item)
{
  SECITEM_FreeItem(item, true);
}

typedef mozilla::pkix::ScopedPtr<SECItem, SECITEM_FreeItem_true> ScopedSECItem;

CERTCertificate*
DecodeBase64Cert(const std::string& base64)
{
  size_t derLen = (base64.length() * 3) / 4;
  if (base64[base64.length() - 1] == '=') {
    derLen--;
  }
  if (base64[base64.length() - 2] == '=') {
    derLen--;
  }
  ScopedSECItem der(SECITEM_AllocItem(nullptr, nullptr, derLen));
  if (!der) {
    PrintPRError("SECITEM_AllocItem failed");
    return nullptr;
  }
  if (!PL_Base64Decode(base64.data(), base64.length(),
                       reinterpret_cast<char*>(der->data))) {
    PrintPRError("PL_Base64Decode failed");
    return nullptr;
  }
  CERTCertificate* cert = CERT_NewTempCertificate(CERT_GetDefaultCertDB(),
                                                  der.get(), nullptr, false,
                                                  true);
  if (!cert) {
    PrintPRError("CERT_NewTempCertificate failed");
    return nullptr;
  }
  return cert;
}

static const char PEM_HEADER[] = "-----BEGIN CERTIFICATE-----";
static const char PEM_FOOTER[] = "-----END CERTIFICATE-----";

CERTCertList*
ReadCertsFromFile(const char* filename)
{
  CERTCertList* certs = CERT_NewCertList();
  if (!certs) {
    PrintPRError("CERT_NewCertList failed");
    return nullptr;
  }
  std::string currentPem;
  bool readingCertificate = false;
  std::ifstream file(filename);
  while (!file.eof()) {
    std::string line;
    std::getline(file, line);
    // getline appears to strip off '\n' but not '\r'
    // (maybe it's platform-dependent?)
    if (line.length() > 0 && line.back() == '\r') {
      line.pop_back();
    }
    if (line.compare(PEM_FOOTER) == 0) {
      readingCertificate = false;
      CERTCertificate* cert = DecodeBase64Cert(currentPem);
      if (cert) {
        if (CERT_AddCertToListTail(certs, cert) != SECSuccess) {
          PrintPRError("CERT_AddCertToListTail failed");
        }
      }
      currentPem.clear();
    }
    if (readingCertificate) {
      currentPem += line;
    }
    if (line.compare(PEM_HEADER) == 0) {
      readingCertificate = true;
    }
  }
  return certs;
}

typedef uint8_t SHA256Buffer[SHA256_LENGTH];

SECStatus
HashBytes(SHA256Buffer& output, const SECItem& data)
{
  if (PK11_HashBuf(SEC_OID_SHA256, output, data.data, data.len)
        != SECSuccess) {
    PrintPRError("PK11_HashBuf failed");
    return SECFailure;
  }
  return SECSuccess;
}

std::ostream&
HexPrint(std::ostream& output)
{
  output.fill('0');
  return output << "0x" << std::hex << std::uppercase << std::setw(2);
}

void
PrintSHA256HashOf(const SECItem& data)
{
  SHA256Buffer hash;
  if (HashBytes(hash, data) != SECSuccess) {
    return;
  }
  // The format is:
  // '{ <11 hex bytes>
  //    <11 hex bytes>
  //    <10 hex bytes> },'
  std::cout << "{ ";
  for (size_t i = 0; i < 11; i++) {
    uint32_t val = hash[i];
    std::cout << HexPrint << val << ", ";
  }
  std::cout << std::endl << "  ";
  for (size_t i = 11; i < 22; i++) {
    uint32_t val = hash[i];
    std::cout << HexPrint << val << ", ";
  }
  std::cout << std::endl << "  ";
  for (size_t i = 22; i < 31; i++) {
    uint32_t val = hash[i];
    std::cout << HexPrint << val << ", ";
  }
  uint32_t val = hash[31];
  std::cout << HexPrint << val << " }," << std::endl;
}

void
PrintBase64Of(const SECItem& data)
{
  std::string base64(PL_Base64Encode(reinterpret_cast<const char*>(data.data),
                                     data.len, nullptr));
  // The format is:
  // '"<base64>"
  //  "<base64>",'
  // where each line is limited to 64 characters of base64 data.
  size_t lines = base64.length() / 64;
  for (size_t line = 0; line < lines; line++) {
    std::cout << "\"" << base64.substr(64 * line, 64) << "\"" << std::endl;
  }
  size_t remainder = base64.length() % 64;
  std::cout << "\"" << base64.substr(base64.length() - remainder) << "\"," << std::endl;
}

typedef mozilla::pkix::ScopedPtr<PLOptState, PL_DestroyOptState>
  ScopedPLOptState;

int main(int argc, char* argv[]) {
  if (argc < 7) {
    PrintUsage(argv[0]);
    return 1;
  }
  if (NSS_NoDB_Init(nullptr) != SECSuccess) {
    PrintPRError("NSS_NoDB_Init failed");
  }
  const char* certsFileName = nullptr;
  const char* dottedOID = nullptr;
  const char* oidDescription = nullptr;
  ScopedPLOptState opt(PL_CreateOptState(argc, argv, "c:o:d:"));
  PLOptStatus os;
  while ((os = PL_GetNextOpt(opt.get())) != PL_OPT_EOL) {
    if (os == PL_OPT_BAD) {
      continue;
    }
    switch (opt->option) {
      case 'c':
        certsFileName = opt->value;
        break;
      case 'o':
        dottedOID = opt->value;
        break;
      case 'd':
        oidDescription = opt->value;
        break;
      default:
        PrintUsage(argv[0]);
        return 1;
    }
  }
  if (!certsFileName || !dottedOID || !oidDescription) {
    PrintUsage(argv[0]);
    return 1;
  }

  RegisterEVCheckerErrors();

  mozilla::pkix::ScopedCERTCertList certs(ReadCertsFromFile(certsFileName));
  if (CERT_LIST_END(CERT_LIST_HEAD(certs), certs)) {
    std::cerr << "Couldn't read certificates from '" << certsFileName << "'";
    std::cerr << std::endl;
    return 1;
  }
  CERTCertificate* root = CERT_LIST_TAIL(certs.get())->cert;
  CERTCertificate* ee = CERT_LIST_HEAD(certs.get())->cert;
  std::cout << "// " << root->issuerName << std::endl;
  std::cout << "\"" << dottedOID << "\"," << std::endl;
  std::cout << "\"" << oidDescription << "\"," << std::endl;
  std::cout << "SEC_OID_UNKNOWN," << std::endl;
  PrintSHA256HashOf(root->derCert);
  PrintBase64Of(root->derIssuer);
  PrintBase64Of(root->serialNumber);
  EVCheckerTrustDomain trustDomain(CERT_DupCertificate(root));
  if (trustDomain.Init(dottedOID, oidDescription) != SECSuccess) {
    return 1;
  }
  mozilla::pkix::ScopedCERTCertList results;
  mozilla::pkix::CertPolicyId evPolicy;
  if (trustDomain.GetFirstEVPolicyForCert(ee, evPolicy)
        != SECSuccess) {
    PrintPRError("GetFirstEVPolicyForCert failed");
    return 1;
  }
  SECStatus rv = BuildCertChain(trustDomain, ee, PR_Now(),
                   mozilla::pkix::EndEntityOrCA::MustBeEndEntity, 0,
                   mozilla::pkix::KeyPurposeId::anyExtendedKeyUsage,
                   evPolicy, nullptr, results);
  if (rv != SECSuccess) {
    PrintPRError("BuildCertChain failed");
    PrintPRErrorString();
    return 1;
  }

  std::cout << "Success!" << std::endl;
  return 0;
}
