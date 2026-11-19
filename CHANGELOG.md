# Changelog

## [0.2.1] - 2026-11-19

- **Fix (CMake):** `nodemailerppConfig.cmake` ab Drogon ko dobara load nahi karta agar wo pehle se loaded hai. Pehle ek project me kai packages saath dhundhne par (khaas kar `find_package(... CONFIG)` bina `REQUIRED` ke) "UUID_lib already exists" jaisa CMake error aa sakta tha. Code me koi badlaav nahi.

## [0.2.0] - 2026-11-19

- **Naya naam:** Chitthi → **nodemailerpp** (Node package ke naam + `pp`, taaki duniya me kisi ko bhi turant samajh aaye)
  - namespace `nodemailer::`, header `<nodemailer/nodemailer.hpp>`, CMake `find_package(nodemailerpp)` + target `nodemailerpp::nodemailerpp`
  - purane naam wala code: bas include, namespace aur CMake naam badalo, API wahi hai
- README me "not affiliated" note

## [0.1.0] - 2026-11-19

Pehli release.

- `createTransport()` (SMTP, libcurl): SSL (465) / STARTTLS / requireTLS / ignoreTLS, AUTH PLAIN/LOGIN, timeouts, certificate check
- MIME builder: text/html (`multipart/alternative`), inline images (`multipart/related` + `cid`), attachments (`multipart/mixed`, base64), RFC 2047 me unicode headers, quoted-printable bodies, Bcc sirf envelope me, Message-ID/Date, header injection se bachav
- `sendMail()` / `verify()`: `co_await` (wapas usi IO loop pe) ya `.get()`
- Transport `defaults()`, aur dev transports: json, stream, file (`.eml`)
- nodemailer jaise error codes: EAUTH, ECONNECTION, ETIMEDOUT, ETLS, EENVELOPE, EMESSAGE, ESEND
- `examples/send_mail.cc`; 32 tests (fake SMTP server + Python MIME parser); ASan/UBSan clean
