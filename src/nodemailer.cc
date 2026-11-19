#include "nodemailer/nodemailer.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <condition_variable>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <random>
#include <sstream>
#include <thread>

namespace nodemailer {

namespace {

// ---------- small helpers ----------

std::string trim(const std::string &s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

bool isAscii(const std::string &s) {
    return std::all_of(s.begin(), s.end(), [](char c) { return static_cast<unsigned char>(c) < 128; });
}

std::string randomHex(size_t bytes) {
    static thread_local std::mt19937_64 rng(std::random_device{}());
    static const char *hex = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < bytes; i++) {
        unsigned v = static_cast<unsigned>(rng() & 0xFF);
        s += hex[v >> 4];
        s += hex[v & 15];
    }
    return s;
}

std::string base64(const std::string &in, size_t lineLen = 0) {
    static const char *t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    size_t col = 0;
    auto put = [&](char c) {
        o += c;
        if (lineLen && ++col == lineLen) {
            o += "\r\n";
            col = 0;
        }
    };
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        unsigned n = (static_cast<unsigned char>(in[i]) << 16) | (static_cast<unsigned char>(in[i + 1]) << 8) |
                     static_cast<unsigned char>(in[i + 2]);
        put(t[(n >> 18) & 63]);
        put(t[(n >> 12) & 63]);
        put(t[(n >> 6) & 63]);
        put(t[n & 63]);
    }
    if (i + 1 == in.size()) {
        unsigned n = static_cast<unsigned char>(in[i]) << 16;
        put(t[(n >> 18) & 63]);
        put(t[(n >> 12) & 63]);
        put('=');
        put('=');
    } else if (i + 2 == in.size()) {
        unsigned n = (static_cast<unsigned char>(in[i]) << 16) | (static_cast<unsigned char>(in[i + 1]) << 8);
        put(t[(n >> 18) & 63]);
        put(t[(n >> 12) & 63]);
        put(t[(n >> 6) & 63]);
        put('=');
    }
    if (lineLen && col) o += "\r\n";
    return o;
}

// RFC 2047 encoded-word for headers with non-ASCII text (split so each word stays < 76 chars, on UTF-8 boundaries)
std::string encodeWord(const std::string &s) {
    if (isAscii(s)) return s;
    std::string out;
    size_t i = 0;
    while (i < s.size()) {
        size_t take = 0, j = i;
        while (j < s.size() && take < 39) {
            unsigned char c = static_cast<unsigned char>(s[j]);
            size_t len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4;
            if (take + len > 39 && take > 0) break;
            take += len;
            j += len;
        }
        if (!out.empty()) out += "\r\n ";
        out += "=?UTF-8?B?" + base64(s.substr(i, take)) + "?=";
        i += take;
    }
    return out;
}

// quoted-printable body, CRLF lines, soft breaks at 76
std::string quotedPrintable(const std::string &in) {
    static const char *hex = "0123456789ABCDEF";
    std::string norm;  // normalise line endings to \n first
    for (size_t i = 0; i < in.size(); i++) {
        if (in[i] == '\r' && i + 1 < in.size() && in[i + 1] == '\n') continue;
        norm += in[i] == '\r' ? '\n' : in[i];
    }
    std::string out, line;
    auto flushSoft = [&] {
        out += line + "=\r\n";
        line.clear();
    };
    for (size_t i = 0; i < norm.size(); i++) {
        unsigned char c = static_cast<unsigned char>(norm[i]);
        if (c == '\n') {
            if (!line.empty() && (line.back() == ' ' || line.back() == '\t')) {  // trailing space must be encoded
                char sp = line.back();
                line.pop_back();
                line += sp == ' ' ? "=20" : "=09";
            }
            out += line + "\r\n";
            line.clear();
            continue;
        }
        std::string enc;
        if ((c >= 33 && c <= 126 && c != '=') || c == ' ' || c == '\t')
            enc = std::string(1, static_cast<char>(c));
        else
            enc = std::string("=") + hex[c >> 4] + hex[c & 15];
        if (line.size() + enc.size() > 75) flushSoft();
        line += enc;
    }
    if (!line.empty()) {
        if (line.back() == ' ' || line.back() == '\t') {
            char sp = line.back();
            line.pop_back();
            line += sp == ' ' ? "=20" : "=09";
        }
        out += line;
    }
    return out;
}

std::string guessType(const std::string &filename) {
    static const std::map<std::string, std::string> m = {
        {".pdf", "application/pdf"}, {".png", "image/png"}, {".jpg", "image/jpeg"}, {".jpeg", "image/jpeg"},
        {".gif", "image/gif"}, {".webp", "image/webp"}, {".svg", "image/svg+xml"}, {".txt", "text/plain"},
        {".csv", "text/csv"}, {".html", "text/html"}, {".json", "application/json"}, {".xml", "application/xml"},
        {".zip", "application/zip"}, {".gz", "application/gzip"}, {".ics", "text/calendar"},
        {".doc", "application/msword"},
        {".docx", "application/vnd.openxmlformats-officedocument.wordprocessingml.document"},
        {".xls", "application/vnd.ms-excel"},
        {".xlsx", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"},
        {".pptx", "application/vnd.openxmlformats-officedocument.presentationml.presentation"},
        {".mp3", "audio/mpeg"}, {".mp4", "video/mp4"}};
    std::string ext = std::filesystem::path(filename).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    auto it = m.find(ext);
    return it == m.end() ? "application/octet-stream" : it->second;
}

std::string rfc2822Date() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_r(&t, &tm);
    char b[64];
    std::strftime(b, sizeof b, "%a, %d %b %Y %H:%M:%S %z", &tm);
    return b;
}

std::string headerParamName(const std::string &filename) {  // filename="..." (RFC 2231 if non-ASCII)
    if (isAscii(filename)) {
        std::string safe;
        for (char c : filename) safe += (c == '"' || c == '\\' || c == '\r' || c == '\n') ? '_' : c;
        return "\"" + safe + "\"";
    }
    return "\"" + encodeWord(filename) + "\"";
}

std::string joinAddresses(const std::vector<std::string> &list) {
    std::string out;
    for (const auto &a : list) out += (out.empty() ? "" : ",\r\n ") + Address::parse(a).format();
    return out;
}

// ---------- worker pool + awaitable ----------

class Pool {
  public:
    Pool() {
        for (int i = 0; i < 4; i++) threads_.emplace_back([this] { run(); });
    }
    ~Pool() {
        {
            std::lock_guard<std::mutex> lk(m_);
            stop_ = true;
        }
        cv_.notify_all();
        for (auto &t : threads_) t.join();
    }
    void post(std::function<void()> f) {
        {
            std::lock_guard<std::mutex> lk(m_);
            q_.push_back(std::move(f));
        }
        cv_.notify_one();
    }

  private:
    void run() {
        for (;;) {
            std::function<void()> f;
            {
                std::unique_lock<std::mutex> lk(m_);
                cv_.wait(lk, [this] { return stop_ || !q_.empty(); });
                if (stop_ && q_.empty()) return;
                f = std::move(q_.front());
                q_.pop_front();
            }
            f();
        }
    }
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> q_;
    std::vector<std::thread> threads_;
    bool stop_ = false;
};

Pool &pool() {
    static Pool p;
    return p;
}

void curlInit() {
    static const bool ok = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
    if (!ok) throw Error("libcurl failed to initialise", "ECONNECTION");
}

}  // namespace

// ---------- Address ----------

Address Address::parse(const std::string &text) {
    std::string t = trim(text);
    auto lt = t.rfind('<'), gt = t.rfind('>');
    Address a;
    if (lt != std::string::npos && gt != std::string::npos && gt > lt) {
        a.email = trim(t.substr(lt + 1, gt - lt - 1));
        a.name = trim(t.substr(0, lt));
        if (a.name.size() >= 2 && a.name.front() == '"' && a.name.back() == '"') a.name = a.name.substr(1, a.name.size() - 2);
    } else {
        a.email = t;
    }
    if (a.email.find('@') == std::string::npos || a.email.find_first_of(" \r\n<>,") != std::string::npos)
        throw Error("Invalid email address: " + text, "EENVELOPE");
    return a;
}

std::string Address::format() const {
    if (name.empty()) return email;
    if (!isAscii(name)) return encodeWord(name) + " <" + email + ">";
    bool needQuote = name.find_first_of(",;:<>@()[]\\\".") != std::string::npos;
    std::string n = name;
    if (needQuote) {
        std::string q = "\"";
        for (char c : name) q += (c == '"' || c == '\\') ? std::string("\\") + c : std::string(1, c);
        n = q + "\"";
    }
    return n + " <" + email + ">";
}

// ---------- MIME ----------

std::string buildMessage(const Mail &m, std::string *messageIdOut) {
    if (m.from.empty()) throw Error("Missing \"from\" address", "EENVELOPE");
    if (m.to.empty() && m.cc.empty() && m.bcc.empty()) throw Error("No recipients defined", "EENVELOPE");
    Address from = Address::parse(m.from);
    std::string domain = from.email.substr(from.email.find('@') + 1);
    std::string messageId = m.messageId.empty() ? "<" + randomHex(16) + "@" + domain + ">" : m.messageId;
    if (messageIdOut) *messageIdOut = messageId;

    // load attachments
    std::vector<Attachment> atts = m.attachments;
    for (auto &a : atts) {
        if (!a.path.empty() && a.content.empty()) {
            std::ifstream in(a.path, std::ios::binary);
            if (!in) throw Error("Attachment not found: " + a.path, "EMESSAGE");
            std::stringstream ss;
            ss << in.rdbuf();
            a.content = ss.str();
            if (a.filename.empty()) a.filename = std::filesystem::path(a.path).filename().string();
        }
        if (a.filename.empty()) a.filename = "attachment";
        if (a.contentType.empty()) a.contentType = guessType(a.filename);
    }
    std::vector<const Attachment *> inlines, regular;
    for (const auto &a : atts) (a.cid.empty() || m.html.empty() ? regular : inlines).push_back(&a);

    std::string h;
    auto header = [&](const std::string &k, const std::string &v) { h += k + ": " + v + "\r\n"; };
    header("From", from.format());
    if (!m.to.empty()) header("To", joinAddresses(m.to));
    if (!m.cc.empty()) header("Cc", joinAddresses(m.cc));
    if (!m.replyTo.empty()) header("Reply-To", Address::parse(m.replyTo).format());
    header("Subject", encodeWord(m.subject));
    header("Date", rfc2822Date());
    header("Message-ID", messageId);
    header("MIME-Version", "1.0");
    header("X-Mailer", "nodemailerpp 0.2");
    for (const auto &[k, v] : m.headers) {
        if (k.find_first_of("\r\n:") != std::string::npos || v.find_first_of("\r\n") != std::string::npos)
            throw Error("Invalid header " + k, "EMESSAGE");
        header(k, encodeWord(v));
    }

    auto textPart = [](const std::string &type, const std::string &body) {
        bool plain7 = isAscii(body) && std::none_of(body.begin(), body.end(), [](char c) { return c == '\r'; });
        size_t longest = 0, cur = 0;
        for (char c : body) cur = c == '\n' ? (longest = std::max(longest, cur), 0) : cur + 1;
        longest = std::max(longest, cur);
        std::string p = "Content-Type: " + type + "; charset=utf-8\r\n";
        if (plain7 && longest <= 76) {
            std::string crlf;
            for (char c : body) crlf += c == '\n' ? std::string("\r\n") : std::string(1, c);
            return p + "Content-Transfer-Encoding: 7bit\r\n\r\n" + crlf;
        }
        return p + "Content-Transfer-Encoding: quoted-printable\r\n\r\n" + quotedPrintable(body);
    };
    auto attachmentPart = [](const Attachment &a, bool isInline) {
        std::string p = "Content-Type: " + a.contentType + "; name=" + headerParamName(a.filename) + "\r\n";
        p += "Content-Transfer-Encoding: base64\r\n";
        p += std::string("Content-Disposition: ") + (isInline ? "inline" : "attachment") +
             "; filename=" + headerParamName(a.filename) + "\r\n";
        if (!a.cid.empty()) p += "Content-ID: <" + a.cid + ">\r\n";
        return p + "\r\n" + base64(a.content, 76);
    };
    auto multipart = [](const std::string &subtype, const std::vector<std::string> &parts) {
        std::string b = "----=_nodemailerpp_" + randomHex(12);
        std::string p = "Content-Type: multipart/" + subtype + "; boundary=\"" + b + "\"\r\n\r\n";
        for (const auto &part : parts) p += "--" + b + "\r\n" + part + "\r\n";
        return p + "--" + b + "--\r\n";
    };

    // body tree: mixed( alternative( text, related( html, inline* ) ), attachment* )
    std::string htmlPart;
    if (!m.html.empty()) {
        htmlPart = textPart("text/html", m.html);
        if (!inlines.empty()) {
            std::vector<std::string> rel{htmlPart};
            for (auto *a : inlines) rel.push_back(attachmentPart(*a, true));
            htmlPart = multipart("related", rel);
        }
    }
    std::string content;
    if (!m.text.empty() && !htmlPart.empty())
        content = multipart("alternative", {textPart("text/plain", m.text), htmlPart});
    else if (!htmlPart.empty())
        content = htmlPart;
    else
        content = textPart("text/plain", m.text);
    if (!regular.empty()) {
        std::vector<std::string> mixed{content};
        for (auto *a : regular) mixed.push_back(attachmentPart(*a, false));
        content = multipart("mixed", mixed);
    }
    return h + content;
}

// ---------- Pending ----------

void Pending::await_suspend(std::coroutine_handle<> h) {
    trantor::EventLoop *loop = trantor::EventLoop::getEventLoopOfCurrentThread();
    pool().post([this, h, loop] {
        try {
            info_ = job_();
        } catch (...) {
            err_ = std::current_exception();
        }
        if (loop)
            loop->queueInLoop([h] { h.resume(); });
        else
            h.resume();
    });
}

Info Pending::await_resume() {
    if (err_) std::rethrow_exception(err_);
    return std::move(*info_);
}

// ---------- Transporter ----------

Pending Transporter::sendMail(Mail mail) const {
    const Mail &d = defaults_;
    if (mail.from.empty()) mail.from = d.from;
    if (mail.replyTo.empty()) mail.replyTo = d.replyTo;
    if (mail.subject.empty()) mail.subject = d.subject;
    if (mail.to.empty()) mail.to = d.to;
    if (mail.cc.empty()) mail.cc = d.cc;
    if (mail.bcc.empty()) mail.bcc = d.bcc;
    for (const auto &kv : d.headers) mail.headers.push_back(kv);
    auto fn = send_;
    return Pending([fn, mail] { return fn(mail); });
}

Pending Transporter::verify() const {
    auto v = verify_;
    return Pending([v] {
        if (v) v();
        return Info{};
    });
}

// ---------- SMTP via libcurl ----------

namespace {

struct ReadState {
    const std::string *data;
    size_t pos = 0;
};

size_t readCb(char *buf, size_t size, size_t n, void *userp) {
    auto *st = static_cast<ReadState *>(userp);
    size_t room = size * n, left = st->data->size() - st->pos, take = std::min(room, left);
    std::memcpy(buf, st->data->data() + st->pos, take);
    st->pos += take;
    return take;
}

struct CurlHandle {
    CURL *c;
    curl_slist *rcpt = nullptr;
    char err[CURL_ERROR_SIZE] = {0};
    CurlHandle() : c(curl_easy_init()) {
        if (!c) throw Error("libcurl: could not create a handle", "ECONNECTION");
    }
    ~CurlHandle() {
        if (rcpt) curl_slist_free_all(rcpt);
        curl_easy_cleanup(c);
    }
};

void baseSetup(CurlHandle &h, const SmtpOptions &o) {
    std::string url = std::string(o.secure ? "smtps://" : "smtp://") + o.host + ":" + std::to_string(o.port);
    if (!o.name.empty()) url += "/" + o.name;  // EHLO name
    curl_easy_setopt(h.c, CURLOPT_URL, url.c_str());
    if (!o.secure && !o.ignoreTLS) curl_easy_setopt(h.c, CURLOPT_USE_SSL, o.requireTLS ? CURLUSESSL_ALL : CURLUSESSL_TRY);
    if (!o.user.empty()) {
        curl_easy_setopt(h.c, CURLOPT_USERNAME, o.user.c_str());
        curl_easy_setopt(h.c, CURLOPT_PASSWORD, o.pass.c_str());
    }
    curl_easy_setopt(h.c, CURLOPT_SSL_VERIFYPEER, o.rejectUnauthorized ? 1L : 0L);
    curl_easy_setopt(h.c, CURLOPT_SSL_VERIFYHOST, o.rejectUnauthorized ? 2L : 0L);
    curl_easy_setopt(h.c, CURLOPT_CONNECTTIMEOUT_MS, o.connectionTimeoutMs);
    curl_easy_setopt(h.c, CURLOPT_TIMEOUT_MS, o.timeoutMs);
    curl_easy_setopt(h.c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(h.c, CURLOPT_ERRORBUFFER, h.err);
}

[[noreturn]] void throwCurl(CURLcode rc, const CurlHandle &h) {
    std::string msg = h.err[0] ? h.err : curl_easy_strerror(rc);
    std::string code = "ESEND";
    switch (rc) {
        case CURLE_LOGIN_DENIED: code = "EAUTH"; break;
        case CURLE_COULDNT_CONNECT:
        case CURLE_COULDNT_RESOLVE_HOST: code = "ECONNECTION"; break;
        case CURLE_OPERATION_TIMEDOUT: code = "ETIMEDOUT"; break;
        case CURLE_SSL_CONNECT_ERROR:
        case CURLE_USE_SSL_FAILED:
        case CURLE_PEER_FAILED_VERIFICATION: code = "ETLS"; break;
        case CURLE_SEND_ERROR:
        case CURLE_RECV_ERROR: code = "ECONNECTION"; break;
        default: break;
    }
    if (rc == CURLE_SEND_ERROR || rc == CURLE_RECV_ERROR || rc == CURLE_WEIRD_SERVER_REPLY) {
        // libcurl reports a refused RCPT TO as a failed send with the server's reply in the message
        std::string lower = msg;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        if (lower.find("rcpt") != std::string::npos) code = "EENVELOPE";
        if (lower.find("auth") != std::string::npos || lower.find("login") != std::string::npos) code = "EAUTH";
    }
    throw Error(msg, code);
}

}  // namespace

Transporter createTransport(SmtpOptions o) {
    auto send = [o](const Mail &m) {
        curlInit();
        Info info;
        std::string msg = buildMessage(m, &info.messageId);
        std::string envFrom = m.sender.empty() ? Address::parse(m.from).email : Address::parse(m.sender).email;
        CurlHandle h;
        baseSetup(h, o);
        std::string mailFrom = "<" + envFrom + ">";
        curl_easy_setopt(h.c, CURLOPT_MAIL_FROM, mailFrom.c_str());
        std::vector<std::string> rcpts;
        for (const auto *list : {&m.to, &m.cc, &m.bcc})
            for (const auto &a : *list) rcpts.push_back(Address::parse(a).email);
        for (const auto &r : rcpts) h.rcpt = curl_slist_append(h.rcpt, ("<" + r + ">").c_str());
        curl_easy_setopt(h.c, CURLOPT_MAIL_RCPT, h.rcpt);
        ReadState rs{&msg};
        curl_easy_setopt(h.c, CURLOPT_READFUNCTION, readCb);
        curl_easy_setopt(h.c, CURLOPT_READDATA, &rs);
        curl_easy_setopt(h.c, CURLOPT_UPLOAD, 1L);
        CURLcode rc = curl_easy_perform(h.c);
        if (rc != CURLE_OK) throwCurl(rc, h);
        long code = 0;
        curl_easy_getinfo(h.c, CURLINFO_RESPONSE_CODE, &code);
        info.accepted = rcpts;
        info.response = std::to_string(code) + " OK";
        return info;
    };
    auto verify = [o] {
        curlInit();
        CurlHandle h;
        baseSetup(h, o);
        curl_easy_setopt(h.c, CURLOPT_CUSTOMREQUEST, "NOOP");
        curl_easy_setopt(h.c, CURLOPT_NOBODY, 1L);
        CURLcode rc = curl_easy_perform(h.c);
        if (rc != CURLE_OK) throwCurl(rc, h);
    };
    return Transporter(send, verify);
}

Transporter createStreamTransport() {
    return Transporter([](const Mail &m) {
        Info info;
        info.raw = buildMessage(m, &info.messageId);
        for (const auto *list : {&m.to, &m.cc, &m.bcc})
            for (const auto &a : *list) info.accepted.push_back(Address::parse(a).email);
        return info;
    });
}

Transporter createJsonTransport() {
    return Transporter([](const Mail &m) {
        Info info;
        buildMessage(m, &info.messageId);  // validates
        Json::Value j;
        j["messageId"] = info.messageId;
        j["from"] = m.from;
        for (const auto &a : m.to) j["to"].append(a);
        for (const auto &a : m.cc) j["cc"].append(a);
        for (const auto &a : m.bcc) j["bcc"].append(a);
        if (!m.replyTo.empty()) j["replyTo"] = m.replyTo;
        j["subject"] = m.subject;
        if (!m.text.empty()) j["text"] = m.text;
        if (!m.html.empty()) j["html"] = m.html;
        for (const auto &a : m.attachments) {
            Json::Value x;
            x["filename"] = a.filename.empty() ? std::filesystem::path(a.path).filename().string() : a.filename;
            if (!a.path.empty()) x["path"] = a.path;
            if (!a.cid.empty()) x["cid"] = a.cid;
            x["contentType"] = a.contentType.empty() ? guessType(x["filename"].asString()) : a.contentType;
            j["attachments"].append(x);
        }
        for (const auto &[k, v] : m.headers) j["headers"][k] = v;
        for (const auto *list : {&m.to, &m.cc, &m.bcc})
            for (const auto &a : *list) info.accepted.push_back(Address::parse(a).email);
        info.json = j;
        return info;
    });
}

Transporter createFileTransport(std::string folder) {
    return Transporter([folder](const Mail &m) {
        Info info;
        info.raw = buildMessage(m, &info.messageId);
        std::filesystem::create_directories(folder);
        std::string id = info.messageId.substr(1, info.messageId.find('@') - 1);
        info.path = (std::filesystem::path(folder) / (id + ".eml")).string();
        std::ofstream(info.path, std::ios::binary) << info.raw;
        for (const auto *list : {&m.to, &m.cc, &m.bcc})
            for (const auto &a : *list) info.accepted.push_back(Address::parse(a).email);
        return info;
    });
}

}  // namespace nodemailer
