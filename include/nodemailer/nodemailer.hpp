#pragma once
/* nodemailerpp ✉️: nodemailer for C++. Builds proper MIME emails and sends them over SMTP (libcurl).

     auto mailer = nodemailer::createTransport({
         .host = "smtp.gmail.com", .port = 465, .secure = true,
         .user = "me@gmail.com", .pass = "app-password",
     });

     auto info = co_await mailer.sendMail({
         .from = "Asha <me@gmail.com>",
         .to = {"ravi@example.com", "Priya <priya@example.com>"},
         .subject = "Namaste 🙏",
         .text = "Plain text version",
         .html = "<h1>Namaste</h1><img src=\"cid:logo\">",
         .attachments = {{.filename = "report.pdf", .path = "files/report.pdf"},
                         {.filename = "logo.png", .path = "logo.png", .cid = "logo"}},
     });
     info.messageId;   // "<...@gmail.com>"

     mailer.sendMail(mail).get();         // blocking (scripts); never inside a server IO loop
     co_await mailer.verify();            // checks connection + login

   Development transports (nothing is sent):
     nodemailer::createJsonTransport()        info.json = the whole message as JSON
     nodemailer::createStreamTransport()      info.raw  = the finished RFC 5322 message
     nodemailer::createFileTransport("mails") writes every mail as mails/<id>.eml (open it in any mail app) */

#include <trantor/net/EventLoop.h>
#include <json/json.h>

#include <coroutine>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace nodemailer {

using Pairs = std::vector<std::pair<std::string, std::string>>;

struct Address {
    std::string name;
    std::string email;
    static Address parse(const std::string &text);  // "Asha <a@b.c>" | "a@b.c" | "\"Kumar, Ravi\" <r@b.c>"
    std::string format() const;                      // header form, non-ASCII names encoded
};

struct Attachment {
    std::string filename;
    std::string content;      // the bytes (use this OR path)
    std::string path;         // read from disk at send time
    std::string contentType;  // "" = guessed from filename
    std::string cid;          // inline image: <img src="cid:...">
};

struct Mail {
    std::string from;
    std::vector<std::string> to, cc, bcc;
    std::string replyTo;
    std::string subject;
    std::string text;
    std::string html;
    std::vector<Attachment> attachments;
    Pairs headers;            // extra headers
    std::string messageId;    // "" = generated
    std::string sender;       // envelope MAIL FROM ("" = from's email)
};

struct Info {
    std::string messageId;
    std::vector<std::string> accepted;  // envelope recipients the server took
    std::string response;               // SMTP transport: libcurl / server result text
    std::string raw;                    // stream / file transport: the message
    Json::Value json;                   // json transport
    std::string path;                   // file transport: where it was written
};

class Error : public std::runtime_error {
  public:
    Error(const std::string &m, std::string code) : std::runtime_error(m), code_(std::move(code)) {}
    // "EAUTH" (login failed) | "ECONNECTION" | "ETIMEDOUT" | "ETLS" | "EENVELOPE" (bad / rejected recipients)
    // | "EMESSAGE" (bad message, e.g. missing attachment) | "ESEND"
    const std::string &code() const { return code_; }

  private:
    std::string code_;
};

struct SmtpOptions {
    std::string host = "localhost";
    int port = 587;
    bool secure = false;        // true: TLS from the start (port 465). false: STARTTLS when the server offers it
    bool requireTLS = false;    // fail instead of sending in plain text when STARTTLS is not available
    bool ignoreTLS = false;     // never use STARTTLS (local test servers)
    std::string user, pass;
    bool rejectUnauthorized = true;  // verify the server certificate
    long connectionTimeoutMs = 10000;
    long timeoutMs = 60000;          // whole send
    std::string name;                // EHLO name ("" = libcurl default)
};

// Builds the RFC 5322 / MIME message (what goes over the wire). Exposed for tests and custom transports.
std::string buildMessage(const Mail &mail, std::string *messageIdOut = nullptr);

class Transporter;

// co_await it (continues on the awaiting IO loop), or .get() to block
class [[nodiscard]] Pending {
  public:
    explicit Pending(std::function<Info()> job) : job_(std::move(job)) {}
    bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<> h);
    Info await_resume();
    Info get() { return job_(); }

  private:
    std::function<Info()> job_;
    std::optional<Info> info_;
    std::exception_ptr err_;
};

class Transporter {
  public:
    using SendFn = std::function<Info(const Mail &)>;
    explicit Transporter(SendFn fn, std::function<void()> verifyFn = nullptr)
        : send_(std::move(fn)), verify_(std::move(verifyFn)) {}

    Pending sendMail(Mail mail) const;
    Pending verify() const;  // SMTP: connect, TLS, login, NOOP. Others: no-op

    // defaults merged into every mail (nodemailer's second createTransport argument)
    Transporter &defaults(Mail d) {
        defaults_ = std::move(d);
        return *this;
    }

  private:
    SendFn send_;
    std::function<void()> verify_;
    Mail defaults_;
};

Transporter createTransport(SmtpOptions o);
Transporter createJsonTransport();
Transporter createStreamTransport();
Transporter createFileTransport(std::string folder);

}  // namespace nodemailer
