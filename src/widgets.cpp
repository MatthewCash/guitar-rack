#include "widgets.h"

#include <QApplication>
#include <QComboBox>
#include <QDial>
#include <QFile>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLocalSocket>
#include <QPalette>
#include <QPainter>
#include <QPen>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSettings>
#include <QSlider>
#include <QStandardPaths>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>
#include <algorithm>
#include <cmath>

namespace {

constexpr int kBooleanHint = 0x001;
constexpr int kControlsPerRow = 7;

class ParameterDial final : public QDial {
public:
    explicit ParameterDial(int size, QWidget *parent = nullptr) : QDial(parent), size_(size)
    {
        setFixedSize(size, size);
        setRange(0, 1000);
        setNotchesVisible(false);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);

        const QPointF center = rect().center();
        const double arcRadius = size_ * 0.39;
        const QRectF arcRect(center.x() - arcRadius, center.y() - arcRadius, arcRadius * 2, arcRadius * 2);
        QPen arcPen(QColor(8, 8, 8, 190), size_ * 0.085, Qt::SolidLine, Qt::RoundCap);
        painter.setPen(arcPen);
        painter.drawArc(arcRect, 225 * 16, -270 * 16);

        const double progress = (value() - minimum()) / static_cast<double>(maximum() - minimum());
        arcPen.setColor(palette().color(QPalette::Active, QPalette::Highlight));
        painter.setPen(arcPen);
        painter.drawArc(arcRect, 225 * 16, std::lround(-270 * 16 * progress));

        const double faceRadius = size_ * 0.28;
        QLinearGradient face(center.x(), center.y() - faceRadius,
                             center.x(), center.y() + faceRadius);
        face.setColorAt(0, QColor(92, 92, 92, 245));
        face.setColorAt(0.48, QColor(36, 36, 36, 250));
        face.setColorAt(1, QColor(8, 8, 8, 245));
        painter.setBrush(face);
        painter.setPen(QPen(QColor(0, 0, 0, 220), 1));
        painter.drawEllipse(center, faceRadius, faceRadius);

        const double angle = (225.0 - 270.0 * progress) * std::acos(-1.0) / 180.0;
        const QPointF start(center.x() + std::cos(angle) * size_ * 0.1,
                            center.y() - std::sin(angle) * size_ * 0.1);
        const QPointF end(center.x() + std::cos(angle) * size_ * 0.23,
                          center.y() - std::sin(angle) * size_ * 0.23);
        painter.setPen(QPen(QColor(240, 240, 240, 235), size_ * 0.045,
                            Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(start, end);
    }

private:
    int size_;
};

class TunerDisplay final : public QWidget {
public:
    explicit TunerDisplay(QWidget *parent = nullptr) : QWidget(parent)
    {
        setMinimumWidth(260);
        setFixedHeight(54);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setToolTip(QStringLiteral("Input tuner, A4 = 440 Hz"));
    }

    void setFrequency(double frequency)
    {
        frequency_ = frequency;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(palette().color(QPalette::Active, QPalette::Text));

        QString note = QStringLiteral("--");
        QString frequencyText = QStringLiteral("-- HZ");
        QString centsText = QStringLiteral("WAITING");
        double cents = 0.0;
        const bool active = frequency_ >= 20.0;
        if (active) {
            static const QStringList notes = {
                QStringLiteral("C"), QStringLiteral("C#"), QStringLiteral("D"),
                QStringLiteral("D#"), QStringLiteral("E"), QStringLiteral("F"),
                QStringLiteral("F#"), QStringLiteral("G"), QStringLiteral("G#"),
                QStringLiteral("A"), QStringLiteral("A#"), QStringLiteral("B")};
            const double midi = 69.0 + 12.0 * std::log2(frequency_ / 440.0);
            const int nearest = std::lround(midi);
            cents = std::clamp((midi - nearest) * 100.0, -50.0, 50.0);
            const int noteIndex = ((nearest % 12) + 12) % 12;
            note = notes.at(noteIndex) + QString::number(nearest / 12 - 1);
            frequencyText = QStringLiteral("%1 HZ").arg(frequency_, 0, 'f', 1);
            centsText = std::abs(cents) <= 2.0
                ? QStringLiteral("IN TUNE")
                : QStringLiteral("%1%2 CENTS")
                      .arg(cents > 0.0 ? QStringLiteral("+") : QString())
                      .arg(cents, 0, 'f', 1);
        }

        QFont noteFont = painter.font();
        noteFont.setPointSize(17);
        noteFont.setBold(true);
        painter.setFont(noteFont);
        painter.drawText(QRect(2, 0, 80, 29), Qt::AlignCenter, note);
        QFont frequencyFont = painter.font();
        frequencyFont.setPointSize(7);
        frequencyFont.setBold(false);
        painter.setFont(frequencyFont);
        painter.drawText(QRect(2, 28, 80, 12), Qt::AlignCenter, frequencyText);
        painter.setPen(std::abs(cents) <= 2.0 && active
                           ? QColor(90, 210, 135)
                           : palette().color(QPalette::Active, QPalette::Text));
        painter.drawText(QRect(2, 40, 80, 12), Qt::AlignCenter, centsText);

        const int left = 90;
        const int right = width() - 7;
        const int center = (left + right) / 2;
        const int y = 28;
        const int halfWidth = right - center;
        auto markerPosition = [center, halfWidth](double value) {
            const double magnitude = std::abs(value);
            const double normalized = magnitude <= 10.0
                ? magnitude / 10.0 * 0.4
                : 0.4 + (magnitude - 10.0) / 40.0 * 0.6;
            return center + std::lround(std::copysign(normalized, value) * halfWidth);
        };

        const int inTuneLeft = markerPosition(-2.0);
        const int inTuneRight = markerPosition(2.0);
        painter.fillRect(QRect(inTuneLeft, 8, inTuneRight - inTuneLeft, 39),
                         QColor(90, 210, 135, 35));
        painter.setPen(QPen(QColor(150, 150, 150, 150), 2));
        painter.drawLine(left, y, right, y);
        painter.drawLine(left, 20, left, 36);
        painter.drawLine(right, 20, right, 36);
        for (double tick : {-25.0, -10.0, 10.0, 25.0}) {
            const int x = markerPosition(tick);
            const int height = std::abs(tick) == 10.0 ? 15 : 10;
            painter.drawLine(x, y - height / 2, x, y + height / 2);
        }
        painter.setPen(QPen(QColor(220, 220, 220, 210), 2));
        painter.drawLine(center, 8, center, 47);

        const int marker = markerPosition(cents);
        const QColor markerColor = active && std::abs(cents) <= 2.0
            ? QColor(90, 210, 135)
            : palette().color(QPalette::Active, QPalette::Highlight);
        painter.setPen(QPen(markerColor, 4,
                            Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(marker, 9, marker, 46);
    }

private:
    double frequency_{};
};

class RackWindow final : public QWidget {
public:
    explicit RackWindow(QString socketPath) : socketPath_(std::move(socketPath))
    {
        setWindowTitle(QStringLiteral("Guitar Rack"));
        setObjectName(QStringLiteral("rack"));
        resize(650, 687);

        auto *outer = new QVBoxLayout(this);
        outer->setContentsMargins(0, 0, 0, 0);
        outer->setSpacing(9);

        auto *header = new QFrame;
        header->setObjectName(QStringLiteral("header"));
        header->setAttribute(Qt::WA_StyledBackground);
        auto *headerLayout = new QHBoxLayout(header);
        headerLayout->setContentsMargins(0, 5, 0, 5);
        headerLayout->setSpacing(5);

        tuner_ = new TunerDisplay;
        headerLayout->addWidget(tuner_, 1);

        auto *masterControl = new QWidget;
        masterControl->setObjectName(QStringLiteral("masterControl"));
        auto *masterLayout = new QVBoxLayout(masterControl);
        masterLayout->setContentsMargins(6, 2, 6, 2);
        masterLayout->setSpacing(2);
        auto *masterHeading = new QHBoxLayout;
        masterHeading->setSpacing(5);
        auto *masterLabel = new QLabel(QStringLiteral("MASTER"));
        status_ = new QLabel(QStringLiteral("STARTING"));
        status_->setObjectName(QStringLiteral("status"));
        status_->setAlignment(Qt::AlignCenter);
        masterValue_ = new QLabel(QStringLiteral("0.0 DB"));
        masterValue_->setObjectName(QStringLiteral("value"));
        masterValue_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        masterHeading->addWidget(masterLabel);
        masterHeading->addStretch();
        masterHeading->addWidget(status_);
        masterHeading->addStretch();
        masterHeading->addWidget(masterValue_);
        masterSlider_ = new QSlider(Qt::Horizontal);
        masterSlider_->setObjectName(QStringLiteral("masterSlider"));
        masterSlider_->setRange(0, 1000);
        masterSlider_->setValue(std::lround(50.0 / 54.0 * 1000.0));
        masterSlider_->setFixedHeight(25);
        connect(masterSlider_, &QSlider::valueChanged, this, [this](int position) {
            const double value = -50.0 + 54.0 * position / 1000.0;
            masterValue_->setText(QStringLiteral("%1 DB").arg(value, 0, 'f', 1));
        });
        auto *masterCommit = new QTimer(masterSlider_);
        masterCommit->setSingleShot(true);
        masterCommit->setInterval(35);
        connect(masterSlider_, &QSlider::valueChanged, masterCommit,
                qOverload<>(&QTimer::start));
        connect(masterCommit, &QTimer::timeout, this, [this] {
            const double value = -50.0 + 54.0 * masterSlider_->value() / 1000.0;
            send({{QStringLiteral("type"), QStringLiteral("set_master_volume")},
                  {QStringLiteral("value"), value}});
        });
        masterLayout->addLayout(masterHeading);
        masterLayout->addWidget(masterSlider_);
        headerLayout->addWidget(masterControl, 1);

        catalog_ = new QComboBox;
        catalog_->setMinimumWidth(145);
        catalog_->setFixedHeight(24);
        catalog_->hide();
        headerLayout->addWidget(catalog_);

        auto *add = new QPushButton(QStringLiteral("ADD"));
        add->setFixedSize(36, 24);
        connect(add, &QPushButton::clicked, this, [this] {
            if (catalog_->currentIndex() < 0) return;
            send({{QStringLiteral("type"), QStringLiteral("add")},
                  {QStringLiteral("uri"), catalog_->currentData().toString()},
                  {QStringLiteral("name"), catalog_->currentText()}});
        });
        add->hide();
        headerLayout->addWidget(add);

        auto *import = new QPushButton(QStringLiteral("IMPORT"));
        import->setFixedSize(52, 24);
        import->setToolTip(QStringLiteral("Import my stuff/main from Guitarix"));
        connect(import, &QPushButton::clicked, this, [this] {
            send({{QStringLiteral("type"), QStringLiteral("import_guitarix")}});
        });
        import->hide();
        headerLayout->addWidget(import);
        auto *save = new QPushButton(QStringLiteral("SAVE"));
        save->setFixedSize(40, 24);
        connect(save, &QPushButton::clicked, this, [this] {
            send({{QStringLiteral("type"), QStringLiteral("save")}});
        });
        save->hide();
        headerLayout->addWidget(save);
        outer->addWidget(header);

        auto *presetPanel = new QFrame;
        presetPanel->setObjectName(QStringLiteral("presetPanel"));
        presetPanel->setAttribute(Qt::WA_StyledBackground);
        auto *presetLayout = new QHBoxLayout(presetPanel);
        presetLayout->setContentsMargins(8, 4, 8, 4);
        presetLayout->setSpacing(5);
        auto addPreset = [this, presetLayout](QPushButton *&button, const QString &label,
                                              const QString &preset) {
            button = new QPushButton(label);
            button->setObjectName(QStringLiteral("presetChoice"));
            button->setFixedHeight(34);
            connect(button, &QPushButton::clicked, this, [this, preset] {
                send({{QStringLiteral("type"), QStringLiteral("set_preset")},
                      {QStringLiteral("bank"), QStringLiteral("my stuff")},
                      {QStringLiteral("preset"), preset}});
            });
            presetLayout->addWidget(button, 1);
        };
        addPreset(electricPreset_, QStringLiteral("ELECTRIC"), QStringLiteral("main"));
        addPreset(acousticPreset_, QStringLiteral("ACOUSTIC"), QStringLiteral("Acoustic"));
        addPreset(bassPreset_, QStringLiteral("BASS"), QStringLiteral("Rock Bass"));
        outer->addWidget(presetPanel);

        content_ = new QWidget;
        content_->setObjectName(QStringLiteral("content"));
        rackLayout_ = new QVBoxLayout(content_);
        rackLayout_->setContentsMargins(0, 0, 0, 0);
        rackLayout_->setSpacing(9);
        auto *enginePanel = new QFrame;
        enginePanel->setObjectName(QStringLiteral("plugin"));
        enginePanel->setAttribute(Qt::WA_StyledBackground);
        auto *engineLayout = new QVBoxLayout(enginePanel);
        engineLayout->setContentsMargins(14, 12, 14, 14);
        engineLayout->setSpacing(5);
        auto *engineTitle = new QLabel(QStringLiteral("GUITARIX DSP ENGINE"));
        engineTitle->setObjectName(QStringLiteral("pluginName"));
        QFont engineTitleFont = engineTitle->font();
        engineTitleFont.setBold(true);
        engineTitle->setFont(engineTitleFont);
        engineTitle->setAlignment(Qt::AlignCenter);
        engineDetail_ = new QLabel(QStringLiteral("STARTING my stuff/main"));
        engineDetail_->setObjectName(QStringLiteral("engineDetail"));
        engineDetail_->setAlignment(Qt::AlignCenter);
        engineLayout->addWidget(engineTitle);
        engineLayout->addWidget(engineDetail_);
        rackLayout_->addWidget(enginePanel);
        rackLayout_->addStretch();

        auto *scroll = new QScrollArea;
        scroll->setObjectName(QStringLiteral("scroll"));
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scroll->setWidget(content_);
        outer->addWidget(scroll, 1);

        connect(&socket_, &QLocalSocket::readyRead, this, [this] { readMessages(); });
        connect(&socket_, &QLocalSocket::connected, this, [this] {
            status_->setText(QStringLiteral("CONNECTING"));
            if (engineDetail_) engineDetail_->setText(QStringLiteral("CONNECTING"));
            tuner_->setToolTip(QStringLiteral("Connecting to audio engine"));
            send({{QStringLiteral("type"), QStringLiteral("get_state")}});
        });
        connect(&socket_, &QLocalSocket::disconnected, this, [this] {
            status_->setText(QStringLiteral("OFFLINE"));
            if (engineDetail_) engineDetail_->setText(QStringLiteral("OFFLINE"));
            tuner_->setFrequency(0.0);
            tuner_->setToolTip(QStringLiteral("Audio engine offline"));
        });

        reconnect_.setInterval(750);
        connect(&reconnect_, &QTimer::timeout, this, [this] {
            if (socket_.state() == QLocalSocket::UnconnectedState) {
                socket_.connectToServer(socketPath_);
            }
        });
        reconnect_.start();
        socket_.connectToServer(socketPath_);
    }

private:
    static void restyle(QWidget *widget)
    {
        widget->style()->unpolish(widget);
        widget->style()->polish(widget);
        widget->update();
    }

    void send(const QJsonObject &command)
    {
        if (socket_.state() != QLocalSocket::ConnectedState) return;
        socket_.write(QJsonDocument(command).toJson(QJsonDocument::Compact));
        socket_.write("\n");
        socket_.flush();
    }

    void readMessages()
    {
        buffer_.append(socket_.readAll());
        while (true) {
            const qsizetype newline = buffer_.indexOf('\n');
            if (newline < 0) return;
            const QByteArray line = buffer_.left(newline);
            buffer_.remove(0, newline + 1);
            QJsonParseError error;
            const auto document = QJsonDocument::fromJson(line, &error);
            if (error.error == QJsonParseError::NoError && document.isObject()) {
                handleMessage(document.object());
            }
        }
    }

    void handleMessage(const QJsonObject &message)
    {
        const QString type = message.value(QStringLiteral("type")).toString();
        if (type == QStringLiteral("snapshot")) {
            tuner_->setToolTip(QStringLiteral("Input tuner, A4 = 440 Hz"));
            rebuildCatalog(message.value(QStringLiteral("catalog")).toArray());
            const auto state = message.value(QStringLiteral("state")).toObject();
            const double masterVolume = state.value(QStringLiteral("master_volume_db")).toDouble();
            masterSlider_->blockSignals(true);
            masterSlider_->setValue(std::lround((masterVolume + 50.0) / 54.0 * 1000.0));
            masterSlider_->blockSignals(false);
            masterValue_->setText(QStringLiteral("%1 DB").arg(masterVolume, 0, 'f', 1));
            const QString preset = state.value(QStringLiteral("preset")).toString();
            const QList<QPair<QPushButton *, QString>> presetButtons = {
                {electricPreset_, QStringLiteral("my stuff/main")},
                {acousticPreset_, QStringLiteral("my stuff/Acoustic")},
                {bassPreset_, QStringLiteral("my stuff/Rock Bass")},
            };
            for (const auto &[button, name] : presetButtons) {
                button->setProperty("active", preset == name);
                restyle(button);
            }
            rebuildRack(state.value(QStringLiteral("plugins")).toArray());
        } else if (type == QStringLiteral("tuner")) {
            tuner_->setFrequency(message.value(QStringLiteral("frequency")).toDouble());
        } else if (type == QStringLiteral("status")) {
            const QString state = message.value(QStringLiteral("status")).toString();
            const QString detail = message.value(QStringLiteral("detail")).toString();
            status_->setText(state.toUpper());
            if (engineDetail_) engineDetail_->setText(detail);
            tuner_->setToolTip(detail);
        } else if (type == QStringLiteral("error")) {
            const QString error = message.value(QStringLiteral("message")).toString();
            status_->setText(QStringLiteral("ERROR"));
            if (engineDetail_) engineDetail_->setText(error);
            tuner_->setToolTip(error);
        }
    }

    void rebuildCatalog(const QJsonArray &catalog)
    {
        const QString selected = catalog_->currentData().toString();
        catalog_->blockSignals(true);
        catalog_->clear();
        for (const auto &value : catalog) {
            const auto plugin = value.toObject();
            catalog_->addItem(plugin.value(QStringLiteral("name")).toString(),
                              plugin.value(QStringLiteral("uri")).toString());
        }
        const int previous = catalog_->findData(selected);
        if (previous >= 0) catalog_->setCurrentIndex(previous);
        catalog_->blockSignals(false);
    }

    void clearRack()
    {
        while (rackLayout_->count() > 1) {
            auto *item = rackLayout_->takeAt(0);
            delete item->widget();
            delete item;
        }
        engineDetail_ = nullptr;
    }

    void rebuildRack(const QJsonArray &plugins)
    {
        clearRack();
        if (plugins.isEmpty()) {
            auto *empty = new QLabel(QStringLiteral("ADD AN EFFECT OR IMPORT YOUR GUITARIX PRESET"));
            empty->setObjectName(QStringLiteral("empty"));
            empty->setAlignment(Qt::AlignCenter);
            empty->setMinimumHeight(80);
            rackLayout_->insertWidget(0, empty);
            return;
        }
        for (int index = 0; index < plugins.size(); ++index) {
            const auto plugin = plugins.at(index).toObject();
            const bool fixed = plugin.value(QStringLiteral("fixed")).toBool();
            rackLayout_->insertWidget(index, pluginCard(plugin, fixed));
        }
    }

    QWidget *pluginCard(const QJsonObject &plugin, bool fixed)
    {
        const QString id = plugin.value(QStringLiteral("id")).toString();
        const bool bypassed = plugin.value(QStringLiteral("bypassed")).toBool();
        auto *card = new QFrame;
        card->setObjectName(QStringLiteral("plugin"));
        card->setAttribute(Qt::WA_StyledBackground);
        card->setProperty("bypassed", bypassed);
        auto *layout = new QVBoxLayout(card);
        layout->setContentsMargins(8, 6, 8, 8);
        layout->setSpacing(2);

        auto *heading = new QHBoxLayout;
        heading->setSpacing(3);
        auto *name = new QLabel(plugin.value(QStringLiteral("name")).toString());
        name->setObjectName(QStringLiteral("pluginName"));
        QFont nameFont = name->font();
        nameFont.setBold(true);
        name->setFont(nameFont);
        heading->addWidget(name);
        if (fixed) {
            auto *tag = new QLabel(QStringLiteral("FIXED"));
            tag->setObjectName(QStringLiteral("sectionTitle"));
            heading->addWidget(tag);
        }
        heading->addStretch();

        if (!fixed) {
            auto *bypass = new QPushButton(bypassed ? QStringLiteral("OFF") : QStringLiteral("ON"));
            bypass->setFixedSize(38, 22);
            bypass->setToolTip(QStringLiteral("Enable or bypass effect"));
            bypass->setProperty("active", !bypassed);
            connect(bypass, &QPushButton::clicked, this, [this, id, bypassed] {
                send({{QStringLiteral("type"), QStringLiteral("set_bypass")},
                      {QStringLiteral("id"), id}, {QStringLiteral("bypassed"), !bypassed}});
            });
            heading->addWidget(bypass);
        }
        layout->addLayout(heading);

        const auto parameters = plugin.value(QStringLiteral("parameters")).toArray();
        layout->addWidget(pluginControls(plugin.value(QStringLiteral("uri")).toString(), id, parameters));
        return card;
    }

    static QJsonObject findParameter(const QJsonArray &parameters, const QString &symbol)
    {
        for (const auto &value : parameters) {
            const auto parameter = value.toObject();
            const QString actual = parameter.value(QStringLiteral("symbol")).toString();
            if (actual.compare(symbol, Qt::CaseInsensitive) == 0
                || actual.endsWith(QLatin1Char('.') + symbol, Qt::CaseInsensitive)) {
                return parameter;
            }
        }
        return {};
    }

    static QJsonObject findGeneratedParameter(const QJsonArray &parameters, const QString &base)
    {
        for (const auto &value : parameters) {
            const auto parameter = value.toObject();
            const QString symbol = parameter.value(QStringLiteral("symbol")).toString();
            if (symbol.startsWith(base + QLatin1Char('_'))
                || symbol.endsWith(QLatin1Char('.') + base, Qt::CaseInsensitive)) {
                return parameter;
            }
        }
        return {};
    }

    static bool hiddenParameter(const QJsonObject &parameter)
    {
        const QString symbol = parameter.value(QStringLiteral("symbol")).toString();
        return symbol == QStringLiteral("BYPASS")
            || symbol.startsWith(QStringLiteral("port_fhash_"))
            || symbol.contains(QStringLiteral("meter"), Qt::CaseInsensitive)
            || QRegularExpression(QStringLiteral("^V\\d+$")).match(symbol).hasMatch();
    }

    QWidget *parameterControl(const QJsonObject &parameter, const QString &id,
                              int dialSize = 69, const QString &displayName = {})
    {
        if (parameter.isEmpty()) return new QWidget;
        const int parameterIndex = parameter.value(QStringLiteral("index")).toInt();
        const int hints = parameter.value(QStringLiteral("hints")).toInt();
        const double minimum = parameter.value(QStringLiteral("minimum")).toDouble();
        const double maximum = parameter.value(QStringLiteral("maximum")).toDouble();
        const double value = parameter.value(QStringLiteral("value")).toDouble();
        auto *control = new QWidget;
        control->setObjectName(QStringLiteral("parameterControl"));
        control->setFixedWidth(std::max(60, dialSize + 12));
        auto *controlLayout = new QVBoxLayout(control);
        controlLayout->setContentsMargins(1, 0, 1, 1);
        controlLayout->setSpacing(0);
        const QString name = displayName.isEmpty()
            ? parameter.value(QStringLiteral("name")).toString() : displayName;
        auto *label = new QLabel(name);
        label->setToolTip(parameter.value(QStringLiteral("symbol")).toString());
        label->setAlignment(Qt::AlignCenter);
        controlLayout->addWidget(label);

        if (hints & kBooleanHint) {
            auto *toggle = new QPushButton;
            toggle->setCheckable(true);
            toggle->setChecked(value > (minimum + maximum) / 2.0);
            toggle->setText(toggle->isChecked() ? QStringLiteral("ON") : QStringLiteral("OFF"));
            toggle->setProperty("active", toggle->isChecked());
            toggle->setFixedSize(50, 27);
            connect(toggle, &QPushButton::toggled, this,
                    [this, toggle, id, parameterIndex, minimum, maximum](bool checked) {
                toggle->setText(checked ? QStringLiteral("ON") : QStringLiteral("OFF"));
                toggle->setProperty("active", checked);
                restyle(toggle);
                sendParameter(id, parameterIndex, checked ? maximum : minimum);
            });
            controlLayout->addWidget(toggle, 0, Qt::AlignHCenter);
        } else {
            auto *dial = new ParameterDial(dialSize);
            const double width = maximum - minimum;
            dial->setValue(width > 0.0 ? std::lround((value - minimum) * 1000.0 / width) : 0);
            auto *number = new QLabel(QString::number(value, 'g', 4));
            number->setObjectName(QStringLiteral("value"));
            number->setAlignment(Qt::AlignCenter);
            connect(dial, &QDial::valueChanged, this, [number, minimum, width](int position) {
                number->setText(QString::number(minimum + width * position / 1000.0, 'g', 4));
            });
            auto *commit = new QTimer(dial);
            commit->setSingleShot(true);
            commit->setInterval(35);
            connect(dial, &QDial::valueChanged, commit, qOverload<>(&QTimer::start));
            connect(commit, &QTimer::timeout, this,
                    [this, dial, id, parameterIndex, minimum, width] {
                sendParameter(id, parameterIndex, minimum + width * dial->value() / 1000.0);
            });
            controlLayout->addWidget(dial, 0, Qt::AlignHCenter);
            controlLayout->addWidget(number);
        }
        return control;
    }

    QWidget *selectorControl(const QJsonObject &parameter, const QString &id,
                             const QStringList &options, int width = 100)
    {
        auto *control = new QWidget;
        control->setObjectName(QStringLiteral("parameterControl"));
        control->setFixedWidth(width);
        auto *layout = new QVBoxLayout(control);
        layout->setContentsMargins(2, 0, 2, 1);
        layout->setSpacing(2);
        auto *label = new QLabel(parameter.value(QStringLiteral("name")).toString());
        label->setAlignment(Qt::AlignCenter);
        auto *selector = new QComboBox;
        selector->addItems(options);
        selector->setCurrentIndex(parameter.value(QStringLiteral("value")).toInt());
        selector->setFixedHeight(28);
        const int index = parameter.value(QStringLiteral("index")).toInt();
        connect(selector, &QComboBox::currentIndexChanged, this,
                [this, id, index](int value) { sendParameter(id, index, value); });
        layout->addWidget(label);
        layout->addWidget(selector);
        return control;
    }

    QWidget *verticalControl(const QJsonObject &parameter, const QString &id, const QString &name)
    {
        const int parameterIndex = parameter.value(QStringLiteral("index")).toInt();
        const double minimum = parameter.value(QStringLiteral("minimum")).toDouble();
        const double maximum = parameter.value(QStringLiteral("maximum")).toDouble();
        const double value = parameter.value(QStringLiteral("value")).toDouble();
        const double width = maximum - minimum;
        auto *control = new QWidget;
        control->setObjectName(QStringLiteral("eqControl"));
        control->setFixedWidth(49);
        auto *layout = new QVBoxLayout(control);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(1);
        auto *label = new QLabel(name);
        label->setAlignment(Qt::AlignCenter);
        auto *slider = new QSlider(Qt::Vertical);
        slider->setRange(0, 1000);
        slider->setValue(width > 0.0 ? std::lround((value - minimum) * 1000.0 / width) : 0);
        slider->setFixedSize(40, 108);
        auto *number = new QLabel(QString::number(value, 'g', 3));
        number->setObjectName(QStringLiteral("value"));
        number->setAlignment(Qt::AlignCenter);
        connect(slider, &QSlider::valueChanged, this, [number, minimum, width](int position) {
            number->setText(QString::number(minimum + width * position / 1000.0, 'g', 3));
        });
        auto *commit = new QTimer(slider);
        commit->setSingleShot(true);
        commit->setInterval(35);
        connect(slider, &QSlider::valueChanged, commit, qOverload<>(&QTimer::start));
        connect(commit, &QTimer::timeout, this,
                [this, slider, id, parameterIndex, minimum, width] {
            sendParameter(id, parameterIndex, minimum + width * slider->value() / 1000.0);
        });
        layout->addWidget(label);
        layout->addWidget(slider, 0, Qt::AlignHCenter);
        layout->addWidget(number);
        return control;
    }

    static QFrame *section(const QString &title, QLayout *content)
    {
        auto *frame = new QFrame;
        frame->setObjectName(QStringLiteral("controlSection"));
        frame->setAttribute(Qt::WA_StyledBackground);
        auto *layout = new QVBoxLayout(frame);
        layout->setContentsMargins(4, 3, 4, 4);
        layout->setSpacing(1);
        auto *label = new QLabel(title);
        label->setObjectName(QStringLiteral("sectionTitle"));
        layout->addWidget(label);
        layout->addLayout(content);
        return frame;
    }

    QWidget *pluginControls(const QString &uri, const QString &id, const QJsonArray &parameters)
    {
        auto *panel = new QWidget;
        panel->setObjectName(QStringLiteral("controlsPanel"));

        if (uri.contains(QStringLiteral("gx_cabinet"))) {
            auto *row = new QHBoxLayout(panel);
            row->setContentsMargins(0, 0, 0, 0);
            row->setSpacing(4);
            row->addWidget(selectorControl(findParameter(parameters, QStringLiteral("c_model")), id,
                {QStringLiteral("1x12"), QStringLiteral("2x12"), QStringLiteral("4x12"),
                 QStringLiteral("4x10"), QStringLiteral("2x10"), QStringLiteral("4x10 Bass"),
                 QStringLiteral("2x15"), QStringLiteral("1x15"), QStringLiteral("Mesa"),
                 QStringLiteral("High Gain"), QStringLiteral("Twin"), QStringLiteral("Marshall"),
                 QStringLiteral("Bassman"), QStringLiteral("JCM800"), QStringLiteral("Jensen"),
                 QStringLiteral("V30"), QStringLiteral("Alnico"), QStringLiteral("1x8"), QStringLiteral("Off")}, 150));
            row->addWidget(parameterControl(findParameter(parameters, QStringLiteral("CBass")), id, 60, QStringLiteral("Bass")));
            row->addWidget(parameterControl(findParameter(parameters, QStringLiteral("CTreble")), id, 60, QStringLiteral("Treble")));
            row->addStretch();
            row->addWidget(parameterControl(findParameter(parameters, QStringLiteral("CLevel")), id, 96, QStringLiteral("Level")));
            return panel;
        }

        if (uri.contains(QStringLiteral("gx_reverb_stereo"))) {
            auto *row = new QHBoxLayout(panel);
            row->setContentsMargins(0, 0, 0, 0);
            row->setSpacing(4);
            row->addWidget(selectorControl(findParameter(parameters, QStringLiteral("mode")), id,
                                           {QStringLiteral("Linear"), QStringLiteral("Ping Pong")}, 115));
            row->addWidget(parameterControl(findParameter(parameters, QStringLiteral("lfo")), id, 57, QStringLiteral("LFO")));
            row->addStretch();
            row->addWidget(parameterControl(findParameter(parameters, QStringLiteral("roomsize")), id, 96, QStringLiteral("Room Size")));
            row->addStretch();
            row->addWidget(parameterControl(findParameter(parameters, QStringLiteral("damp")), id, 63, QStringLiteral("Damp")));
            row->addWidget(parameterControl(findParameter(parameters, QStringLiteral("dry_wet")), id, 69, QStringLiteral("Dry / Wet")));
            return panel;
        }

        if (uri.contains(QStringLiteral("gx_graphiceq"))
            || uri == QStringLiteral("guitarix-internal:graphiceq")) {
            static const QStringList frequencies = {
                QStringLiteral("<31"), QStringLiteral("62"), QStringLiteral("125"),
                QStringLiteral("250"), QStringLiteral("500"), QStringLiteral("1k"),
                QStringLiteral("2k"), QStringLiteral("4k"), QStringLiteral("8k"),
                QStringLiteral("16k"), QStringLiteral(">16k")};
            auto *row = new QHBoxLayout(panel);
            row->setContentsMargins(5, 0, 5, 0);
            row->setSpacing(2);
            for (int band = 0; band < frequencies.size(); ++band) {
                row->addWidget(verticalControl(findParameter(parameters, QStringLiteral("G%1").arg(band + 1)),
                                               id, frequencies.at(band)));
            }
            return panel;
        }

        if (uri.contains(QStringLiteral("gx_distortion"))) {
            auto *root = new QVBoxLayout(panel);
            root->setContentsMargins(0, 0, 0, 0);
            root->setSpacing(2);

            auto *bands = new QHBoxLayout;
            bands->setSpacing(1);
            const QStringList symbols = {
                QStringLiteral("low"), QStringLiteral("middle_l"),
                QStringLiteral("middle_h"), QStringLiteral("high")};
            const QStringList labels = {
                QStringLiteral("LOW"), QStringLiteral("LOW MID"),
                QStringLiteral("HIGH MID"), QStringLiteral("HIGH")};
            for (int band = 0; band < symbols.size(); ++band) {
                auto *column = new QVBoxLayout;
                column->setSpacing(0);
                auto *bandLabel = new QLabel(labels.at(band));
                bandLabel->setAlignment(Qt::AlignCenter);
                column->addWidget(bandLabel);
                column->addWidget(parameterControl(
                    findGeneratedParameter(parameters, symbols.at(band) + QStringLiteral("_drive")),
                    id, 54, QStringLiteral("Drive")));
                column->addWidget(parameterControl(
                    findGeneratedParameter(parameters, symbols.at(band) + QStringLiteral("_gain")),
                    id, 54, QStringLiteral("Gain")));
                bands->addLayout(column);
            }
            root->addWidget(section(QStringLiteral("BANDS"), bands));

            auto *master = new QHBoxLayout;
            master->setSpacing(2);
            master->addWidget(parameterControl(findGeneratedParameter(parameters, QStringLiteral("drive")), id, 86, QStringLiteral("Drive")));
            master->addWidget(parameterControl(findGeneratedParameter(parameters, QStringLiteral("gain")), id, 72, QStringLiteral("Gain")));
            master->addWidget(parameterControl(findGeneratedParameter(parameters, QStringLiteral("level")), id, 72, QStringLiteral("Level")));
            master->addStretch();
            master->addWidget(parameterControl(findGeneratedParameter(parameters, QStringLiteral("wet_dry")), id, 86, QStringLiteral("Wet / Dry")));
            root->addWidget(section(QStringLiteral("MASTER / MIX"), master));

            auto *detail = new QHBoxLayout;
            detail->setSpacing(2);
            detail->addWidget(parameterControl(findGeneratedParameter(parameters, QStringLiteral("split_low_freq")), id, 54, QStringLiteral("Low Split")));
            detail->addWidget(parameterControl(findGeneratedParameter(parameters, QStringLiteral("split_middle_freq")), id, 54, QStringLiteral("Mid Split")));
            detail->addWidget(parameterControl(findGeneratedParameter(parameters, QStringLiteral("split_high_freq")), id, 54, QStringLiteral("High Split")));
            detail->addStretch();
            detail->addWidget(parameterControl(findGeneratedParameter(parameters, QStringLiteral("resonator_on_off")), id, 54, QStringLiteral("Resonator")));
            detail->addWidget(parameterControl(findGeneratedParameter(parameters, QStringLiteral("trigger")), id, 54, QStringLiteral("Trigger")));
            detail->addWidget(parameterControl(findGeneratedParameter(parameters, QStringLiteral("vibrato")), id, 54, QStringLiteral("Vibrato")));
            root->addWidget(section(QStringLiteral("SPLIT / RESONATOR"), detail));
            return panel;
        }

        if (uri.contains(QStringLiteral("gx_mbdistortion"))) {
            auto *root = new QVBoxLayout(panel);
            root->setContentsMargins(0, 0, 0, 0);
            root->setSpacing(2);
            auto *top = new QHBoxLayout;
            top->setSpacing(3);
            auto *bands = new QHBoxLayout;
            bands->setSpacing(1);
            for (int band = 1; band <= 5; ++band) {
                auto *column = new QVBoxLayout;
                column->setSpacing(0);
                auto *bandLabel = new QLabel(QStringLiteral("B%1").arg(band));
                bandLabel->setAlignment(Qt::AlignCenter);
                column->addWidget(bandLabel);
                column->addWidget(parameterControl(findParameter(parameters, QStringLiteral("DRIVE%1").arg(band)), id, 54, QStringLiteral("Drive")));
                column->addWidget(parameterControl(findParameter(parameters, QStringLiteral("OFFSET%1").arg(band)), id, 54, QStringLiteral("Offset")));
                bands->addLayout(column);
            }
            top->addWidget(section(QStringLiteral("BANDS"), bands), 1);
            auto *gain = new QHBoxLayout;
            gain->addWidget(parameterControl(findParameter(parameters, QStringLiteral("GAIN")), id, 102, QStringLiteral("Gain")));
            top->addWidget(section(QStringLiteral("MASTER"), gain));
            root->addLayout(top);
            auto *crossovers = new QHBoxLayout;
            crossovers->setSpacing(3);
            for (int band = 1; band <= 4; ++band) {
                crossovers->addWidget(parameterControl(
                    findParameter(parameters, QStringLiteral("CROSSOVER_B%1_B%2").arg(band).arg(band + 1)),
                    id, 51, QStringLiteral("B%1 / B%2").arg(band).arg(band + 1)));
            }
            crossovers->addStretch();
            root->addWidget(section(QStringLiteral("CROSSOVER"), crossovers));
            return panel;
        }

        if (uri == QStringLiteral("guitarix-internal:ampstack")) {
            auto *row = new QHBoxLayout(panel);
            row->setContentsMargins(0, 0, 0, 0);
            row->setSpacing(0);
            row->addStretch();
            row->addWidget(parameterControl(
                findParameter(parameters, QStringLiteral("gxdistortion.drive")), id, 86,
                QStringLiteral("Drive")));
            row->addStretch();
            row->addWidget(parameterControl(
                findParameter(parameters, QStringLiteral("gxdistortion.wet_dry")), id, 72,
                QStringLiteral("Wet / Dry")));
            row->addStretch();
            row->addWidget(parameterControl(
                findParameter(parameters, QStringLiteral("amp2.stage1.Pregain")), id, 72,
                QStringLiteral("Stage 1")));
            row->addStretch();
            row->addWidget(parameterControl(
                findParameter(parameters, QStringLiteral("amp2.stage2.gain1")), id, 72,
                QStringLiteral("Stage 2")));
            row->addStretch();
            row->addWidget(parameterControl(
                findParameter(parameters, QStringLiteral("tube.select")), id, 63,
                QStringLiteral("Tube")));
            row->addStretch();
            return panel;
        }

        if (uri == QStringLiteral("https://faustlv2.bitbucket.io/gxamp_stereo")) {
            auto *row = new QHBoxLayout(panel);
            row->setContentsMargins(0, 0, 0, 0);
            row->setSpacing(0);
            row->addStretch();
            row->addWidget(parameterControl(
                findGeneratedParameter(parameters, QStringLiteral("_gxdistortion_drive")), id, 86,
                QStringLiteral("Drive")));
            row->addStretch();
            row->addWidget(parameterControl(
                findGeneratedParameter(parameters, QStringLiteral("_gxdistortion_wet_dry")), id, 72,
                QStringLiteral("Wet / Dry")));
            row->addStretch();
            row->addWidget(parameterControl(
                findGeneratedParameter(parameters, QStringLiteral("_amp2_stage1_Pregain")), id, 72,
                QStringLiteral("Stage 1")));
            row->addStretch();
            row->addWidget(parameterControl(
                findGeneratedParameter(parameters, QStringLiteral("_amp2_stage2_gain1")), id, 72,
                QStringLiteral("Stage 2")));
            row->addStretch();
            return panel;
        }

        if (uri == QStringLiteral("guitarix-internal:mxrdis")) {
            auto *row = new QHBoxLayout(panel);
            row->setContentsMargins(0, 0, 0, 0);
            row->setSpacing(0);
            row->addStretch();
            row->addWidget(parameterControl(
                findParameter(parameters, QStringLiteral("mxrdis.drive")), id, 86,
                QStringLiteral("Drive")));
            row->addStretch();
            row->addWidget(parameterControl(
                findParameter(parameters, QStringLiteral("mxrdis.Volume")), id, 86,
                QStringLiteral("Volume")));
            row->addStretch();
            return panel;
        }

        if (uri == QStringLiteral("https://faustlv2.bitbucket.io/bassbooster")) {
            auto *row = new QHBoxLayout(panel);
            row->setContentsMargins(0, 0, 0, 0);
            row->setSpacing(4);
            row->addWidget(parameterControl(
                findGeneratedParameter(parameters, QStringLiteral("_bassbooster_Level")), id, 96,
                QStringLiteral("Bass Boost")));
            row->addStretch();
            return panel;
        }

        if (uri == QStringLiteral("http://tomszilagyi.github.io/plugins/lv2/ir")) {            auto *root = new QVBoxLayout(panel);
            root->setContentsMargins(0, 0, 0, 0);
            root->setSpacing(2);
            auto sectionRow = [this, &parameters, &id](const QStringList &symbols, int size = 54) {
                auto *row = new QHBoxLayout;
                row->setSpacing(3);
                for (const QString &symbol : symbols) row->addWidget(parameterControl(findParameter(parameters, symbol), id, size));
                row->addStretch();
                return row;
            };
            root->addWidget(section(QStringLiteral("IMPULSE"), sectionRow({
                QStringLiteral("port_reverse"), QStringLiteral("port_predelay"),
                QStringLiteral("port_attack"), QStringLiteral("port_attacktime"),
                QStringLiteral("port_envelope"), QStringLiteral("port_length"),
                QStringLiteral("port_stretch")})));
            auto *lower = new QHBoxLayout;
            lower->setSpacing(2);
            lower->addWidget(section(QStringLiteral("STEREO"), sectionRow({
                QStringLiteral("port_stereo_in"), QStringLiteral("port_stereo_ir"),
                QStringLiteral("port_agc_sw")})));
            lower->addWidget(section(QStringLiteral("MIX"), sectionRow({
                QStringLiteral("port_dry_sw"), QStringLiteral("port_dry_gain"),
                QStringLiteral("port_wet_sw"), QStringLiteral("port_wet_gain")}, 72)), 1);
            root->addLayout(lower);
            return panel;
        }

        QJsonArray visible;
        for (const auto &value : parameters) {
            const auto parameter = value.toObject();
            if (hiddenParameter(parameter)) continue;
            visible.append(parameter);
        }
        auto *root = new QVBoxLayout(panel);
        root->setContentsMargins(0, 0, 0, 0);
        root->setSpacing(2);
        const int count = static_cast<int>(visible.size());
        const int rowCount = (count + kControlsPerRow - 1) / kControlsPerRow;
        const int controlsPerRow = rowCount > 0 ? (count + rowCount - 1) / rowCount : 0;
        for (int first = 0; first < count; first += controlsPerRow) {
            auto *row = new QHBoxLayout;
            row->setSpacing(0);
            row->addStretch();
            const int end = std::min(first + controlsPerRow, count);
            for (int i = first; i < end; ++i) {
                row->addWidget(parameterControl(visible.at(i).toObject(), id, 69));
                row->addStretch();
            }
            root->addLayout(row);
        }
        return panel;
    }

    void sendParameter(const QString &id, int index, double value)
    {
        send({{QStringLiteral("type"), QStringLiteral("set_parameter")},
              {QStringLiteral("id"), id}, {QStringLiteral("index"), index},
              {QStringLiteral("value"), value}});
    }

    QString socketPath_;
    QLocalSocket socket_;
    QTimer reconnect_;
    QByteArray buffer_;
    TunerDisplay *tuner_{};
    QLabel *status_{};
    QLabel *engineDetail_{};
    QSlider *masterSlider_{};
    QLabel *masterValue_{};
    QPushButton *electricPreset_{};
    QPushButton *acousticPreset_{};
    QPushButton *bassPreset_{};
    QComboBox *catalog_{};
    QWidget *content_{};
    QVBoxLayout *rackLayout_{};
};

} // namespace

int run_guitar_rack(const std::string &socket_path)
{
    qputenv("QT_STYLE_OVERRIDE", "Fusion");
    int argc = 1;
    char applicationName[] = "guitar-rack";
    char *argv[] = {applicationName, nullptr};
    QApplication application(argc, argv);
    application.setApplicationName(QStringLiteral("guitar-rack"));
    application.setDesktopFileName(QStringLiteral("guitar-rack"));

    QSettings kdeGlobals(
        QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
            + QStringLiteral("/kdeglobals"),
        QSettings::IniFormat);
    const QStringList accentComponents = kdeGlobals.value(QStringLiteral("AccentColor"))
                                             .toStringList();
    if (accentComponents.size() == 3) {
        bool redValid = false;
        bool greenValid = false;
        bool blueValid = false;
        const int red = accentComponents.at(0).toInt(&redValid);
        const int green = accentComponents.at(1).toInt(&greenValid);
        const int blue = accentComponents.at(2).toInt(&blueValid);
        if (redValid && greenValid && blueValid) {
            QPalette palette = application.palette();
            const QColor accent(red, green, blue);
            palette.setColor(QPalette::Active, QPalette::Highlight, accent);
            palette.setColor(QPalette::Inactive, QPalette::Highlight, accent);
            application.setPalette(palette);
        }
    }

    const QColor highlight = application.palette().color(QPalette::Active, QPalette::Highlight);
    const QString highlightCss = QStringLiteral("rgba(%1, %2, %3, %4)")
                                     .arg(highlight.red())
                                     .arg(highlight.green())
                                     .arg(highlight.blue())
                                     .arg(highlight.alpha());
    QString stylesheet = QStringLiteral(R"(
        QWidget#rack { background: transparent; }
        QFrame#header, QFrame#plugin { background-color: #40404030; border: none; border-radius: 0px; }
        QFrame#presetPanel { background: transparent; border: none; }
        QWidget#content, QScrollArea#scroll, QScrollArea#scroll > QWidget > QWidget { background: transparent; border: none; }
        QLabel { background: transparent; border: none; color: palette(text); }
        QLabel#pluginName { color: palette(text); font-size: 13px; }
        QLabel#engineDetail { color: rgba(210,210,210,210); }
        QLabel#empty { color: rgba(190,190,190,180); }
        QLabel#value { font-size: 9px; color: palette(text); }
        QWidget#parameterControl { background: transparent; border: none; font-size: 10px; }
        QWidget#eqControl { background: transparent; border: none; font-size: 9px; }
        QWidget#controlsPanel { background: transparent; border: none; }
        QFrame#controlSection { background: transparent; border: none; border-radius: 0px; }
        QLabel#sectionTitle { font-size: 9px; color: palette(text); }
        QFrame#plugin[bypassed="true"] QLabel#pluginName { color: rgb(245, 82, 82); }
        QPushButton, QComboBox {
            background-color: transparent;
            border: none;
            border-radius: 0px;
            padding: 3px 6px;
        }
        QPushButton:hover, QComboBox:hover { background-color: #4a4a4a; }
        QPushButton:pressed, QPushButton[active="true"] { background: __HIGHLIGHT__; color: #ffffff; }
        QPushButton:disabled { background: transparent; color: rgba(130,130,130,120); }
        QPushButton#presetChoice {
            background: rgba(64, 64, 64, 48);
            border: none;
            border-bottom: 6px solid transparent;
            padding: 4px 7px;
        }
        QPushButton#presetChoice:hover { background: rgba(74, 74, 74, 160); }
        QPushButton#presetChoice[active="true"] {
            background: rgba(64, 64, 64, 48);
            color: palette(text);
            border-bottom: 6px solid __HIGHLIGHT__;
        }
        QPushButton#presetChoice[active="true"]:hover { background: rgba(74, 74, 74, 160); }
        QComboBox QAbstractItemView { background: rgb(20,20,20); selection-background-color: __HIGHLIGHT__; }
        QSlider { background: transparent; }
        QSlider#masterSlider::groove:horizontal {
            height: 10px;
            border-radius: 5px;
            background: rgba(8,8,8,170);
            border: 1px solid rgba(255,255,255,48);
        }
        QSlider#masterSlider::sub-page:horizontal {
            border-radius: 5px;
            background: __HIGHLIGHT__;
        }
        QSlider#masterSlider::add-page:horizontal {
            border-radius: 5px;
            background: rgba(5,5,5,165);
        }
        QSlider#masterSlider::handle:horizontal {
            width: 15px;
            margin: -6px 0;
            border-radius: 2px;
            background: qlineargradient(x1:0, y1:0, x2:1, y2:0,
                stop:0 rgba(25,25,25,245),
                stop:0.5 rgba(100,100,100,250),
                stop:1 rgba(25,25,25,245));
            border: 1px solid rgba(0,0,0,210);
        }
        QSlider::groove:vertical {
            width: 12px;
            border-radius: 6px;
            background: rgba(8,8,8,170);
            border: 1px solid rgba(255,255,255,48);
        }
        QSlider::sub-page:vertical { width: 12px; border-radius: 6px; background: rgba(5,5,5,165); }
        QSlider::add-page:vertical { width: 12px; border-radius: 6px; background: __HIGHLIGHT__; }
        QSlider::handle:vertical {
            width: 34px;
            height: 14px;
            margin: 0 -11px;
            border-radius: 0px;
            background: qlineargradient(x1:0, y1:0, x2:0, y2:1,
                stop:0 rgba(92,92,92,245),
                stop:0.45 rgba(36,36,36,245),
                stop:1 rgba(8,8,8,235));
            border: 1px solid rgba(0,0,0,210);
        }
        QScrollBar:vertical { width: 8px; background: transparent; }
        QScrollBar::handle:vertical { background: rgba(180,180,180,90); min-height: 28px; }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
    )");
    application.setStyleSheet(stylesheet.replace(QStringLiteral("__HIGHLIGHT__"), highlightCss));

    const QString path = QString::fromUtf8(socket_path.data(), static_cast<qsizetype>(socket_path.size()));
    RackWindow window(path);
    window.ensurePolished();
    window.setWindowFlag(Qt::FramelessWindowHint);
    window.setAttribute(Qt::WA_TranslucentBackground);
    window.setAttribute(Qt::WA_NoSystemBackground);
    window.setAutoFillBackground(false);
    window.show();
    return application.exec();
}
