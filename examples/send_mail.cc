// Send one mail from the command line, settings from the environment:
//   SMTP_HOST=smtp.gmail.com SMTP_PORT=465 SMTP_SECURE=1 SMTP_USER=me@gmail.com SMTP_PASS=app-password \
//   ./send_mail friend@example.com "Subject" "Body text" [attachment]
// Without SMTP_HOST it writes the mail to ./outbox/<id>.eml instead (open it in any mail app).
#include <nodemailer/nodemailer.hpp>

#include <cstdlib>
#include <iostream>

static std::string env(const char *k, const char *def = "") {
    const char *v = std::getenv(k);
    return v && *v ? v : def;
}

int main(int argc, char **argv) {
    if (argc < 4) {
        std::cerr << "usage: send_mail TO SUBJECT TEXT [ATTACHMENT]\n";
        return 2;
    }
    nodemailer::Transporter mailer = env("SMTP_HOST").empty()
                                      ? nodemailer::createFileTransport("outbox")
                                      : nodemailer::createTransport({.host = env("SMTP_HOST"),
                                                                  .port = std::atoi(env("SMTP_PORT", "587").c_str()),
                                                                  .secure = env("SMTP_SECURE") == "1",
                                                                  .user = env("SMTP_USER"),
                                                                  .pass = env("SMTP_PASS")});
    nodemailer::Mail m{.from = env("SMTP_FROM", env("SMTP_USER", "me@localhost").c_str()), .to = {argv[1]}, .subject = argv[2], .text = argv[3]};
    if (argc > 4) m.attachments.push_back({.path = argv[4]});
    try {
        auto info = mailer.sendMail(m).get();
        std::cout << "sent " << info.messageId << (info.path.empty() ? "" : " -> " + info.path) << "\n";
    } catch (const nodemailer::Error &e) {
        std::cerr << e.code() << ": " << e.what() << "\n";
        return 1;
    }
}
