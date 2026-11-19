// nodemailerpp self-test: a tiny fake SMTP server in this process + real sends through libcurl.
#include <nodemailer/nodemailer.hpp>
#include <drogon/drogon.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <thread>

static int gPass = 0, gFail = 0;
#define CHECK(cond, name)                                                \
    do {                                                                 \
        if (cond) gPass++;                                               \
        else {                                                           \
            gFail++;                                                     \
            std::cout << "  FAIL: " << name << "  (" << #cond << ")\n";  \
        }                                                                \
    } while (0)

// ---------------- fake SMTP server ----------------
struct Received {
    std::string from;
    std::vector<std::string> rcpt;
    std::string data;
    std::string auth;  // decoded "user:pass"
};
static std::mutex gM;
static std::vector<Received> gInbox;
static const int SMTP_PORT = 2526;

static std::string b64decode(const std::string &in) {
    std::string out;
    int val = 0, bits = -8;
    for (char c : in) {
        const char *t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        const char *p = strchr(t, c);
        if (!p || c == 0) continue;
        val = (val << 6) + static_cast<int>(p - t);
        bits += 6;
        if (bits >= 0) {
            out += static_cast<char>((val >> bits) & 0xFF);
            bits -= 8;
        }
    }
    return out;
}

static void session(int fd) {
    auto send = [&](const std::string &s) { (void)!write(fd, s.data(), s.size()); };
    std::string buf;
    auto readLine = [&](std::string &line) {
        for (;;) {
            auto p = buf.find("\r\n");
            if (p != std::string::npos) {
                line = buf.substr(0, p);
                buf.erase(0, p + 2);
                return true;
            }
            char tmp[4096];
            ssize_t n = read(fd, tmp, sizeof tmp);
            if (n <= 0) return false;
            buf.append(tmp, static_cast<size_t>(n));
        }
    };
    send("220 fake.local ESMTP test\r\n");
    Received cur;
    bool authed = false;
    std::string line;
    while (readLine(line)) {
        std::string up = line.substr(0, 10);
        for (auto &c : up) c = static_cast<char>(toupper(c));
        if (up.rfind("EHLO", 0) == 0 || up.rfind("HELO", 0) == 0) {
            send("250-fake.local\r\n250-AUTH PLAIN LOGIN\r\n250-8BITMIME\r\n250 SIZE 10000000\r\n");
        } else if (up.rfind("AUTH PLAIN", 0) == 0) {
            std::string tok = line.size() > 11 ? line.substr(11) : "";
            if (tok.empty()) {
                send("334 \r\n");
                readLine(tok);
            }
            std::string d = b64decode(tok);  // \0user\0pass
            auto a = d.find('\0', 1);
            std::string user = d.substr(1, a - 1), pass = d.substr(a + 1);
            cur.auth = user + ":" + pass;
            if (pass == "secret") {
                authed = true;
                send("235 ok\r\n");
            } else
                send("535 5.7.8 Authentication failed\r\n");
        } else if (up.rfind("AUTH LOGIN", 0) == 0) {
            std::string u, p;
            send("334 VXNlcm5hbWU6\r\n");
            readLine(u);
            send("334 UGFzc3dvcmQ6\r\n");
            readLine(p);
            cur.auth = b64decode(u) + ":" + b64decode(p);
            if (b64decode(p) == "secret") {
                authed = true;
                send("235 ok\r\n");
            } else
                send("535 5.7.8 Authentication failed\r\n");
        } else if (up.rfind("MAIL FROM", 0) == 0) {
            cur.from = line.substr(line.find(':') + 1);
            send("250 ok\r\n");
        } else if (up.rfind("RCPT TO", 0) == 0) {
            std::string r = line.substr(line.find(':') + 1);
            if (r.find("reject@") != std::string::npos)
                send("550 5.1.1 No such user\r\n");
            else {
                cur.rcpt.push_back(r);
                send("250 ok\r\n");
            }
        } else if (up.rfind("DATA", 0) == 0) {
            send("354 go\r\n");
            std::string data, l;
            while (readLine(l) && l != ".") data += (l.rfind("..", 0) == 0 ? l.substr(1) : l) + "\r\n";
            cur.data = data;
            {
                std::lock_guard<std::mutex> lk(gM);
                gInbox.push_back(cur);
            }
            cur = Received{};
            send("250 2.0.0 queued as TEST123\r\n");
        } else if (up.rfind("NOOP", 0) == 0 || up.rfind("RSET", 0) == 0) {
            send("250 ok\r\n");
        } else if (up.rfind("QUIT", 0) == 0) {
            send("221 bye\r\n");
            break;
        } else {
            send("502 unknown\r\n");
        }
    }
    (void)authed;
    close(fd);
}

static void smtpServer(std::atomic<bool> &ready) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(SMTP_PORT);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(s, reinterpret_cast<sockaddr *>(&a), sizeof a) != 0 || listen(s, 16) != 0) {
        std::cout << "cannot start fake SMTP server\n";
        exit(1);
    }
    ready = true;
    for (;;) {
        int c = accept(s, nullptr, nullptr);
        if (c < 0) continue;
        std::thread(session, c).detach();
    }
}

static Received lastMail() {
    std::lock_guard<std::mutex> lk(gM);
    return gInbox.empty() ? Received{} : gInbox.back();
}
static size_t inboxSize() {
    std::lock_guard<std::mutex> lk(gM);
    return gInbox.size();
}

// ---------------- tests ----------------
static nodemailer::SmtpOptions smtpOpts(const std::string &pass = "secret") {
    return {.host = "127.0.0.1", .port = SMTP_PORT, .ignoreTLS = true, .user = "asha@example.com", .pass = pass,
            .connectionTimeoutMs = 3000, .timeoutMs = 10000};
}

static std::string python(const std::string &script, const std::string &file) {
    std::string cmd = "python3 -c '" + script + "' " + file + " 2>&1";
    std::string out;
    if (FILE *p = popen(cmd.c_str(), "r")) {
        char b[512];
        while (fgets(b, sizeof b, p)) out += b;
        pclose(p);
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
}

static void runChecks() {
    std::filesystem::create_directories("nodemailerpp_test");
    std::string pdf = std::string("%PDF-1.4\n") + std::string(300, '\x01') + "binary\xff\xfe end";
    std::ofstream("nodemailerpp_test/report.pdf", std::ios::binary) << pdf;
    std::ofstream("nodemailerpp_test/logo.png", std::ios::binary) << "\x89PNG fake logo";

    auto mailer = nodemailer::createTransport(smtpOpts());

    // full mail: text + html + inline image + attachment + cc/bcc + unicode
    nodemailer::Info info = mailer
                             .sendMail({.from = "Asha Kumari <asha@example.com>",
                                        .to = {"ravi@example.com", "\"Sharma, Priya\" <priya@example.com>"},
                                        .cc = {"मित्र <mitra@example.com>"},
                                        .bcc = {"hidden@example.com"},
                                        .replyTo = "support@example.com",
                                        .subject = "Namaste 🙏 report ready",
                                        .text = "Hello Ravi,\nYour report is attached.\nशुभकामनाएँ\n.dot line\n",
                                        .html = "<h1>Hello</h1><p>See logo: <img src=\"cid:logo1\"></p>",
                                        .attachments = {{.path = "nodemailerpp_test/report.pdf"},
                                                        {.filename = "logo.png", .path = "nodemailerpp_test/logo.png", .cid = "logo1"},
                                                        {.filename = "notes.txt", .content = "inline content"}},
                                        .headers = {{"X-Campaign", "nov"}}})
                             .get();
    Received r = lastMail();
    CHECK(info.messageId.rfind("<", 0) == 0 && info.messageId.find("@example.com>") != std::string::npos, "messageId");
    CHECK(r.auth == "asha@example.com:secret", "SMTP login");
    CHECK(r.from == "<asha@example.com>", "envelope MAIL FROM");
    CHECK(r.rcpt.size() == 4 && r.rcpt[3] == "<hidden@example.com>", "to + cc + bcc in the envelope");
    CHECK(info.accepted.size() == 4, "info.accepted");
    CHECK(r.data.find("Bcc") == std::string::npos && r.data.find("hidden@") == std::string::npos, "Bcc never in the headers");
    CHECK(r.data.find("Subject: =?UTF-8?B?") != std::string::npos, "unicode subject encoded");
    CHECK(r.data.find("\"Sharma, Priya\" <priya@example.com>") != std::string::npos, "quoted display name");
    CHECK(r.data.find("X-Campaign: nov") != std::string::npos, "custom header");

    std::ofstream("nodemailerpp_test/sent.eml", std::ios::binary) << r.data;
    // let Python's email package (a real MIME parser) take the message apart
    std::string py = python(
        "import sys,email\n"
        "from email import policy\n"
        "m=email.message_from_binary_file(open(sys.argv[1],\"rb\"),policy=policy.default)\n"
        "parts=[p.get_content_type() for p in m.walk()]\n"
        "att=[p for p in m.iter_attachments()]\n"
        "pdf=[p for p in att if p.get_filename()==\"report.pdf\"][0].get_content()\n"
        "logo=[p for p in m.walk() if p.get(\"Content-ID\")==\"<logo1>\"][0]\n"
        "body=m.get_body((\"plain\",)).get_content()\n"
        "print(\"|\".join([str(m[\"subject\"]), str(m[\"cc\"]), \",\".join(parts), str(len(pdf)), str(pdf[-6:]==b\"\\xff\\xfe end\"),"
        " logo.get_filename(), str(\"शुभकामनाएँ\" in body), str(\".dot line\" in body), str(m[\"reply-to\"])]))\n",
        "nodemailerpp_test/sent.eml");
    CHECK(py.find("Namaste 🙏 report ready|") == 0, "python: subject decodes back");
    CHECK(py.find("मित्र <mitra@example.com>") != std::string::npos, "python: unicode Cc name");
    CHECK(py.find("multipart/mixed,multipart/alternative,text/plain,multipart/related,text/html,image/png,application/pdf,text/plain") !=
              std::string::npos,
          "python: MIME tree mixed(alternative(text, related(html, inline png)), pdf, txt)");
    CHECK(py.find("|" + std::to_string(pdf.size()) + "|True|") != std::string::npos, "python: binary attachment survives base64");
    CHECK(py.find("|logo.png|True|True|support@example.com") != std::string::npos, "python: inline image, unicode + dot lines in text");
    if (py.find("Namaste") == std::string::npos) std::cout << "  python said: " << py << "\n";

    // simple text-only mail is 7bit, html-only has no alternative part
    mailer.sendMail({.from = "a@example.com", .to = {"b@example.com"}, .subject = "Hi", .text = "short plain"}).get();
    r = lastMail();
    CHECK(r.data.find("Content-Transfer-Encoding: 7bit") != std::string::npos && r.data.find("multipart") == std::string::npos,
          "plain ASCII text -> single 7bit part");
    mailer.sendMail({.from = "a@example.com", .to = {"b@example.com"}, .subject = "Hi", .html = "<b>x</b>"}).get();
    CHECK(lastMail().data.find("Content-Type: text/html") != std::string::npos && lastMail().data.find("multipart") == std::string::npos,
          "html only -> single html part");

    // errors
    auto fail = [](auto fn) -> std::string {
        try {
            fn();
        } catch (const nodemailer::Error &e) {
            return e.code();
        } catch (const std::exception &e) {
            return std::string("other:") + e.what();
        }
        return "none";
    };
    CHECK(fail([] { nodemailer::createTransport(smtpOpts("wrong")).sendMail({.from = "a@x.com", .to = {"b@x.com"}, .text = "x"}).get(); }) == "EAUTH",
          "wrong password -> EAUTH");
    CHECK(fail([&] { mailer.sendMail({.from = "a@x.com", .to = {"reject@x.com"}, .text = "x"}).get(); }) == "EENVELOPE",
          "rejected recipient -> EENVELOPE");
    CHECK(fail([] {
              auto o = smtpOpts();
              o.port = 1;
              nodemailer::createTransport(o).sendMail({.from = "a@x.com", .to = {"b@x.com"}, .text = "x"}).get();
          }) == "ECONNECTION",
          "nothing listening -> ECONNECTION");
    CHECK(fail([&] { mailer.sendMail({.from = "a@x.com", .to = {"b@x.com"}, .attachments = {{.path = "nope.pdf"}}}).get(); }) == "EMESSAGE",
          "missing attachment -> EMESSAGE");
    CHECK(fail([&] { mailer.sendMail({.from = "not-an-email", .to = {"b@x.com"}}).get(); }) == "EENVELOPE", "bad from -> EENVELOPE");
    CHECK(fail([&] { mailer.sendMail({.from = "a@x.com"}).get(); }) == "EENVELOPE", "no recipients -> EENVELOPE");
    CHECK(fail([&] { mailer.sendMail({.from = "a@x.com", .to = {"b@x.com"}, .headers = {{"X-Bad", "a\r\nBcc: evil@x.com"}}}).get(); }) ==
              "EMESSAGE",
          "header injection blocked");
    CHECK(fail([&] { mailer.verify().get(); }) == "none", "verify() ok");
    CHECK(fail([] { nodemailer::createTransport(smtpOpts("wrong")).verify().get(); }) == "EAUTH", "verify() with wrong password");

    // defaults
    auto withDefaults = nodemailer::createTransport(smtpOpts());
    withDefaults.defaults({.from = "App <noreply@example.com>", .headers = {{"X-App", "iot"}}});
    withDefaults.sendMail({.to = {"u@example.com"}, .subject = "d", .text = "x"}).get();
    CHECK(lastMail().data.find("From: App <noreply@example.com>") != std::string::npos && lastMail().data.find("X-App: iot") != std::string::npos,
          "transport defaults");

    // dev transports
    auto json = nodemailer::createJsonTransport().sendMail({.from = "a@x.com", .to = {"b@x.com"}, .subject = "J", .text = "t",
                                                         .attachments = {{.path = "nodemailerpp_test/report.pdf"}}}).get();
    CHECK(json.json["subject"].asString() == "J" && json.json["attachments"][0]["filename"].asString() == "report.pdf" &&
              json.json["attachments"][0]["contentType"].asString() == "application/pdf",
          "json transport");
    auto stream = nodemailer::createStreamTransport().sendMail({.from = "a@x.com", .to = {"b@x.com"}, .subject = "S", .text = "t"}).get();
    CHECK(stream.raw.find("Subject: S\r\n") != std::string::npos && stream.raw.find("MIME-Version: 1.0") != std::string::npos,
          "stream transport");
    auto file = nodemailer::createFileTransport("nodemailerpp_test/outbox").sendMail({.from = "a@x.com", .to = {"b@x.com"}, .text = "t"}).get();
    CHECK(std::filesystem::exists(file.path) && file.path.size() > 4 && file.path.substr(file.path.size() - 4) == ".eml", "file transport");

    // long unicode text -> quoted-printable with soft line breaks; no line over 78 chars
    std::string longText;
    for (int i = 0; i < 40; i++) longText += "नमस्ते दुनिया ";
    auto qp = nodemailer::createStreamTransport().sendMail({.from = "a@x.com", .to = {"b@x.com"}, .text = longText}).get();
    bool shortLines = true;
    size_t pos = 0;
    while (pos < qp.raw.size()) {
        size_t e = qp.raw.find("\r\n", pos);
        if (e == std::string::npos) e = qp.raw.size();
        if (e - pos > 78) shortLines = false;
        pos = e + 2;
    }
    std::ofstream("nodemailerpp_test/qp.eml", std::ios::binary) << qp.raw;
    std::string back = python("import sys,email\nfrom email import policy\nm=email.message_from_binary_file(open(sys.argv[1],\"rb\"),policy=policy.default)\nprint(m.get_content().count(\"नमस्ते\"))",
                              "nodemailerpp_test/qp.eml");
    CHECK(qp.raw.find("quoted-printable") != std::string::npos && shortLines && back == "40", "quoted-printable, 78-col lines, decodes back");
}

// co_await inside a Drogon server handler, resuming on the same IO loop
static std::atomic<int> gWrongThread{0};
static drogon::Task<drogon::HttpResponsePtr> sendHandler(drogon::HttpRequestPtr) {
    auto *loop = trantor::EventLoop::getEventLoopOfCurrentThread();
    auto mailer = nodemailer::createTransport(smtpOpts());
    nodemailer::Mail m{.from = "server@example.com", .to = {"x@example.com"}, .subject = "from handler", .text = "hi"};
    auto info = co_await mailer.sendMail(m);
    if (trantor::EventLoop::getEventLoopOfCurrentThread() != loop) gWrongThread++;
    auto r = drogon::HttpResponse::newHttpResponse();
    r->setBody(info.messageId);
    co_return r;
}

int main() {
    std::atomic<bool> ready{false};
    std::thread(smtpServer, std::ref(ready)).detach();
    while (!ready) std::this_thread::sleep_for(std::chrono::milliseconds(10));

    runChecks();

    drogon::app().registerHandler("/send", &sendHandler);
    drogon::app().addListener("127.0.0.1", 18911).setThreadNum(2);
    std::thread checker([] {
        while (!drogon::app().isRunning()) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        auto client = drogon::HttpClient::newHttpClient("http://127.0.0.1:18911");
        size_t before = inboxSize();
        int ok = 0;
        for (int i = 0; i < 20; i++) {
            auto req = drogon::HttpRequest::newHttpRequest();
            req->setPath("/send");
            auto [res, resp] = client->sendRequest(req, 10);
            if (res == drogon::ReqResult::Ok && resp->statusCode() == drogon::k200OK && resp->body().find('@') != std::string::npos) ok++;
        }
        CHECK(ok == 20 && inboxSize() == before + 20, "co_await sendMail in a server handler (20x)");
        CHECK(gWrongThread == 0, "resumed on the same IO loop");
        drogon::app().quit();
    });
    drogon::app().run();
    checker.join();
    std::filesystem::remove_all("nodemailerpp_test");
    std::cout << gPass << " passed, " << gFail << " failed\n";
    return gFail ? 1 : 0;
}
