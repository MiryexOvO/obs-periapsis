// SPDX-License-Identifier: BSD-3-Clause
// Extend only PERI's server field in the existing OBS settings page.
#include <QApplication>
#include <QComboBox>
#include <QEvent>
#include <QLineEdit>
#include <QPointer>
#include <QSignalBlocker>
#include <QTimer>
#include <QWidget>

namespace {
class Binding : public QObject {
    QPointer<QWidget> page;
    QPointer<QComboBox> service, server;
    QPointer<QLineEdit> key;
    QString originalStyle;
    bool enabled = false;
    void apply() {
        if (!service || !server) return;
        const bool peri = service->currentText() == QStringLiteral("PERI");
        if (!peri) {
            if (enabled) {
                server->setEditable(false);
                server->setStyleSheet(originalStyle);
                enabled = false;
            }
            return;
        }
        if (enabled) return;
        enabled = true;
        QString address = server->currentData().toString();
        if (address == QStringLiteral("peri://invitation") &&
            (!key || !key->text().trimmed().startsWith(QStringLiteral("peri://v1/")))) address.clear();
        {
            QSignalBlocker blocker(server);
            server->setEditable(true);
            server->setCompleter(nullptr);
            server->setInsertPolicy(QComboBox::NoInsert);
            server->setStyleSheet(originalStyle + QStringLiteral(
                " QComboBox::drop-down {width:0px;border:0px;} QComboBox::down-arrow {image:none;}"));
            if (server->currentIndex() < 0) server->addItem(address, address);
            server->setItemText(server->currentIndex(), address);
            server->setItemData(server->currentIndex(), address);
            server->setEditText(address);
            server->lineEdit()->setPlaceholderText(QStringLiteral("peri://IP:端口"));
            server->lineEdit()->setMaxLength(128);
        }
        // OBS saves currentData(), not the editable text. Keep both in sync and
        // emit its existing change signal so Apply/OK use the normal save path.
        connect(server->lineEdit(), &QLineEdit::textEdited, this, [this](const QString &text) {
            if (!enabled || !server || !service || service->currentText() != QStringLiteral("PERI")) return;
            // Updating any model role can make QComboBox reload DisplayRole into
            // its editor. Copy the signal text before touching that model.
            const QString edited = text;
            auto *editor = server->lineEdit();
            const int cursor = editor->cursorPosition();
            const int selection = editor->selectionStart();
            const int selectionLength = editor->selectedText().size();
            const int index = server->currentIndex();
            {
                QSignalBlocker blocker(server);
                QSignalBlocker editorBlocker(editor);
                server->setItemText(index, edited);
                server->setItemData(index, edited.trimmed());
                editor->setText(edited);
                if (selection >= 0) {
                    if (cursor == selection) editor->setSelection(selection + selectionLength, -selectionLength);
                    else editor->setSelection(selection, selectionLength);
                } else editor->setCursorPosition(cursor);
            }
            QMetaObject::invokeMethod(server, "currentIndexChanged", Qt::DirectConnection, Q_ARG(int, index));
        });
    }
public:
    Binding(QWidget *dialog, QComboBox *services, QComboBox *servers, QObject *owner)
        : QObject(owner), page(dialog), service(services), server(servers),
          key(dialog->findChild<QLineEdit *>(QStringLiteral("key"))), originalStyle(servers->styleSheet()) {
        dialog->setProperty("periEditorAttached", true);
        connect(service, &QComboBox::currentTextChanged, this, [this] { apply(); });
        connect(dialog, &QObject::destroyed, this, &QObject::deleteLater);
        apply();
    }
    ~Binding() override {
        if (page) page->setProperty("periEditorAttached", false);
        if (server && enabled) { server->setEditable(false); server->setStyleSheet(originalStyle); }
    }
};
class SettingsFilter : public QObject {
    bool eventFilter(QObject *object, QEvent *event) override {
        if (event->type() == QEvent::Show) {
            auto *dialog = qobject_cast<QWidget *>(object);
            if (dialog && dialog->inherits("OBSBasicSettings") && !dialog->property("periEditorAttached").toBool()) {
                auto *service = dialog->findChild<QComboBox *>(QStringLiteral("service"));
                auto *server = dialog->findChild<QComboBox *>(QStringLiteral("server"));
                if (service && server) new Binding(dialog, service, server, this);
            }
        }
        return false;
    }
public:
    explicit SettingsFilter(QObject *parent) : QObject(parent) {}
};
QPointer<SettingsFilter> filter;
}
void peri_settings_ui_install() {
    if (qApp && !filter) {
        filter = new SettingsFilter(qApp);
        qApp->installEventFilter(filter);
    }
}
void peri_settings_ui_uninstall() {
    if (filter) { if (qApp) qApp->removeEventFilter(filter); delete filter.data(); }
}
