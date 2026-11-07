#include "../native/settings-ui.cpp"
#include <QDialog>
#include <QFormLayout>
#include <cassert>
#include <cstdio>
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QDialog page;
    page.setWindowTitle(QStringLiteral("PERI · OBS 连接设置"));
    auto *layout = new QFormLayout(&page);
    auto *service = new QComboBox(&page);
    auto *server = new QComboBox(&page);
    auto *key = new QLineEdit(&page);
    service->setObjectName("service"); server->setObjectName("server"); key->setObjectName("key");
    key->setEchoMode(QLineEdit::Password);
    service->addItems({"Other", "PERI"});
    layout->addRow(QStringLiteral("服务"), service);
    layout->addRow(QStringLiteral("服务器"), server);
    layout->addRow(QStringLiteral("推流码"), key);
    // Model OBS rebuilding the server list before the extension's service callback.
    QObject::connect(service, &QComboBox::currentTextChanged, &page, [&](const QString &name) {
        server->clear();
        server->addItem(name == "PERI" ? "Legacy" : "Default",
            name == "PERI" ? "peri://invitation" : "rtmp://example.test");
    });
    server->addItem("Default", "rtmp://example.test");
    QObject owner;
    auto *binding = new Binding(&page, service, server, &owner);
    assert(!server->isEditable());
    service->setCurrentText("PERI");
    assert(server->isEditable());
    assert(server->currentText().isEmpty());
    int changes = 0;
    QObject::connect(server, &QComboBox::currentIndexChanged, &page, [&](int) { ++changes; });
    auto edit = [&](const QString &value) {
        server->lineEdit()->setText(value);
        QMetaObject::invokeMethod(server->lineEdit(), "textEdited", Qt::DirectConnection, Q_ARG(QString, value));
    };
    edit("peri://192.0.2.10:4444");
    assert(server->currentData().toString() == "peri://192.0.2.10:4444" && changes == 1);
    edit("peri://43.248.128.243:23927");
    assert(server->currentData().toString() == "peri://43.248.128.243:23927" && changes == 2);
    // Persisted values are reloaded by OBS as a new list item.
    const auto saved = server->currentData().toString();
    delete binding;
    server->clear(); server->addItem(saved, saved);
    binding = new Binding(&page, service, server, &owner);
    assert(server->currentText() == saved && server->currentData().toString() == saved);
    key->setText("peri1_example_hidden_publish_code");
    page.resize(620, 170); layout->activate();
    if (argc > 1) assert(page.grab().save(QString::fromLocal8Bit(argv[1])));
    service->setCurrentText("Other");
    assert(!server->isEditable() && server->currentData().toString() == "rtmp://example.test");
    service->setCurrentText("PERI");
    changes = 0;
    edit("peri://[::1]:4444");
    assert(changes == 1 && server->currentData().toString() == "peri://[::1]:4444");
    delete binding;
    peri_settings_ui_install(); peri_settings_ui_install(); peri_settings_ui_uninstall();
    puts("PERI editable server: save/reload, IPv4/IPv6, service switching and isolation OK");
}
