# nodemailerpp ✉️: C++ ke liye nodemailer

> **Note:** nodemailerpp, [Nodemailer](https://nodemailer.com) se inspired hai aur uske jaisa API deta hai. Ye us project ka official package nahi hai, na hi uske maintainers se juda hai. (pehle iska naam **Chitthi** tha.)

nodemailerpp ek C++20 library hai jo sahi MIME email banati hai aur SMTP se bhejti hai (libcurl ke through), bilkul **nodemailer** ki tarah. Isme text, HTML, attachments, inline images, Hindi/emoji wale subject aur naam sab chalte hain.

**Naam:** package/repo **`nodemailerpp`**, C++ namespace **`nodemailer::`**, header **`<nodemailer/nodemailer.hpp>`**, CMake target **`nodemailerpp::nodemailerpp`**.


```cpp
#include <nodemailer/nodemailer.hpp>

auto mailer = nodemailer::createTransport({
    .host = "smtp.gmail.com", .port = 465, .secure = true,
    .user = "me@gmail.com", .pass = "app-password",
});

auto info = co_await mailer.sendMail({
    .from = "Asha <me@gmail.com>",
    .to = {"ravi@example.com"},
    .subject = "Namaste 🙏",
    .text = "Report attached hai.",
    .html = "<h1>Namaste</h1><p>Report attached hai.</p>",
    .attachments = {{.path = "files/report.pdf"}},
});
info.messageId;   // "<...@gmail.com>"
```

Ye Nevla, Express++ aur axiospp ki tarah ek alag project hai.

## nodemailer → nodemailerpp cheat sheet

| nodemailer (Node) | nodemailerpp (C++) |
|---|---|
| `nodemailer.createTransport({host, port, secure, auth: {user, pass}})` | `nodemailer::createTransport({.host, .port, .secure, .user, .pass})` |
| `await transporter.sendMail({...})` | `co_await mailer.sendMail({...})` ya `.get()` |
| `await transporter.verify()` | `co_await mailer.verify()` |
| `from, to, cc, bcc, replyTo, subject, text, html` | same naam (`to/cc/bcc` = `std::vector<std::string>`) |
| `attachments: [{filename, path}]` | `.attachments = {{.filename = "...", .path = "..."}}` |
| `attachments: [{filename, content}]` | `{.filename = "notes.txt", .content = "bytes..."}` |
| `attachments: [{path, cid: 'logo'}]` + `<img src="cid:logo">` | `{.path = "logo.png", .cid = "logo"}` |
| `headers: {'X-Campaign': 'nov'}` | `.headers = {{"X-Campaign", "nov"}}` |
| `envelope.from` | `.sender = "bounce@example.com"` |
| `info.messageId`, `info.accepted` | `info.messageId`, `info.accepted` |
| `createTransport(opts, defaults)` | `nodemailer::createTransport(opts).defaults({.from = "..."})` |
| `jsonTransport: true` | `nodemailer::createJsonTransport()` |
| `streamTransport: true, buffer: true` | `nodemailer::createStreamTransport()` → `info.raw` |
| `err.code` (`EAUTH`, `ECONNECTION`, ...) | `e.code()` (same codes) |

## Install

```bash
sudo apt install libcurl4-openssl-dev libdrogon-dev libjsoncpp-dev uuid-dev libpq-dev libmariadb-dev libsqlite3-dev libhiredis-dev libyaml-cpp-dev
cmake -S . -B build -G Ninja && cmake --build build
ctest --test-dir build --output-on-failure
sudo cmake --install build
```

```cmake
find_package(nodemailerpp 0.2 CONFIG REQUIRED)
target_link_libraries(myapp PRIVATE nodemailerpp::nodemailerpp)
```

Event loops ke liye Drogon (trantor) chahiye. Mail bhejne ka asli kaam libcurl karta hai.

## SMTP settings

| Option | Default | Matlab |
|---|---|---|
| `host`, `port` | `localhost`, `587` | SMTP server |
| `secure` | `false` | `true` = shuru se TLS (port **465**). `false` = server STARTTLS de to use karo (port **587**) |
| `requireTLS` | `false` | STARTTLS na mile to bhejo hi mat |
| `ignoreTLS` | `false` | kabhi TLS mat use karo (local test servers ke liye) |
| `user`, `pass` | — | login (AUTH PLAIN/LOGIN, jo server de) |
| `rejectUnauthorized` | `true` | server ka certificate check karo |
| `connectionTimeoutMs`, `timeoutMs` | `10000`, `60000` | timeouts |
| `name` | — | EHLO naam |

**Common providers:**
- **Gmail:** `smtp.gmail.com`, 465 + `secure = true`. Password ki jagah Google ka **App Password** lagta hai.
- **Outlook/Office365:** `smtp.office365.com`, 587
- **Zoho:** `smtp.zoho.in`, 465
- **Brevo/SES/Mailgun** jaisi services: unke SMTP host/port/user/pass do.

## Kaise bhejein

```cpp
// server handler me (Express++ / Drogon): co_await, IO loop nahi rukta
app.post("/contact", [mailer](express::Req req, express::Res res) -> express::Task<> {
    nodemailer::Mail m{.from = "site@example.com", .to = {"me@example.com"},
                    .subject = "New message", .text = req.json()["message"].asString()};
    co_await mailer.sendMail(m);
    res.json(express::obj("ok", true));
});

// normal program / script me: .get() bhejne tak ruk jaata hai
mailer.sendMail(m).get();
```

- Mail bhejna network ka kaam hai (100 ms se kuch second tak). Ye nodemailerpp ke apne worker threads pe chalta hai, aur `co_await` ke baad code usi IO loop pe wapas aata hai.
- Server ke andar `.get()` kabhi mat lagana, hamesha `co_await` use karo.

> **GCC 13 tip:** `co_await mailer.sendMail({...braces...})` likhne pe GCC 13 crash karta hai. Upar ke example ki tarah `Mail` pehle variable me banao, phir `co_await` karo.

## Email kaisi banti hai

nodemailerpp wahi structure banati hai jo Gmail/Outlook samajhte hain:

```
multipart/mixed                      (sirf jab attachments hon)
├── multipart/alternative            (jab text aur html dono hon)
│   ├── text/plain
│   └── multipart/related            (jab cid wali inline images hon)
│       ├── text/html
│       └── image/png  (Content-ID: <logo>)
├── application/pdf  (attachment)
└── ...
```

- **Hindi/emoji** subject aur naam RFC 2047 me encode hote hain (`=?UTF-8?B?...?=`). Body UTF-8 me jaati hai: chhote ASCII text ke liye 7bit, baaki ke liye quoted-printable. Lines 78 characters se lambi nahi hoti.
- **Attachments** base64 me jaate hain. Content-Type file extension se pata kiya jaata hai.
- **Bcc** sirf envelope (RCPT TO) me jaata hai, header me kabhi nahi.
- **Message-ID** aur **Date** header apne aap lag jaate hain.
- **Header injection** se bachav hai: kisi header me newline ho to `EMESSAGE` error aata hai.

## Development transports (kuch bheja nahi jaata)

```cpp
auto dev = nodemailer::createFileTransport("outbox");   // har mail outbox/<id>.eml me save, kisi bhi mail app me khulti hai
auto js  = nodemailer::createJsonTransport();           // info.json = poori mail JSON me (tests ke liye)
auto raw = nodemailer::createStreamTransport();         // info.raw = poora RFC 5322 message
```

**Tip:** `.env` me `SMTP_HOST` na ho to file transport use karo. Development me galti se asli users ko mail nahi jayegi. Iska example `examples/send_mail.cc` me hai.

## Errors

```cpp
try {
    co_await mailer.sendMail(m);
} catch (const nodemailer::Error &e) {
    e.code();   // "EAUTH" (galat login) | "ECONNECTION" | "ETIMEDOUT" | "ETLS"
                // "EENVELOPE" (galat/reject hua address, recipient nahi) | "EMESSAGE" (attachment nahi mili, galat header) | "ESEND"
    e.what();   // server / libcurl ka message
}
```

## Tests

`ctest` usi process me ek chhota fake SMTP server chalata hai aur libcurl se asli mail bhejta hai. Isme **32 checks** hain:
- login, envelope (to/cc/bcc), Bcc header me na ho
- Hindi/emoji subject aur naam, quoted display names, custom headers
- Python ke `email` parser se poora MIME tree, binary attachment, inline image, Hindi text aur dot-lines wapas padh ke check
- quoted-printable (78 col), 7bit/html-only parts
- errors: EAUTH, EENVELOPE, ECONNECTION, EMESSAGE, header injection
- `verify()`, defaults, json/stream/file transports
- Drogon server handler ke andar 20 baar `co_await sendMail`, jisme check hota hai ki code usi IO loop pe resume hua

ASan + UBSan pe bhi saare tests clean pass hote hain.

## Abhi ki limits

- TLS (465 / STARTTLS) libcurl+OpenSSL se hota hai. Tests me sirf plain local server use hua hai, asli TLS server pe test nahi hua.
- Kai recipients me se agar ek bhi reject ho, to poori mail fail hoti hai (`EENVELOPE`). nodemailer jaisa "kuch accepted, kuch rejected" abhi nahi hai.
- Connection pooling (ek connection pe kai mails), DKIM signing, OAuth2 login (Gmail XOAUTH2) aur templates abhi nahi hain. Templates ke liye Express++ ka `express::renderTemplate()` use kar sakte ho.
