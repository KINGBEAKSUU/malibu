#include "libyuv.h"

#include <QDir>

#include "selfdrive/ui/qt/util.h"

#include "selfdrive/frogpilot/screenrecorder/screenrecorder.h"

int MAX_DURATION = 1000 * 60 * 5;
int SCREEN_HEIGHT = 1080;
int SCREEN_WIDTH = 2160;

ScreenRecorder::ScreenRecorder(QWidget *parent) : QPushButton(parent) {
  setFixedSize(btn_size, btn_size);

  recordingsFolder = "/data/media/screen_recordings";

  rgbScaleBuffer.resize(SCREEN_WIDTH * SCREEN_HEIGHT * 4);

  QObject::connect(this, &QPushButton::clicked, this, &ScreenRecorder::toggleRecording);
  QObject::connect(uiState(), &UIState::offroadTransition, this, &ScreenRecorder::stopRecording);
  QObject::connect(uiState(), &UIState::uiUpdate, this, &ScreenRecorder::updateState);
}

ScreenRecorder::~ScreenRecorder() {
  stopRecording();
}

void ScreenRecorder::updateState() {
  if (!recording) {
    return;
  }

  if (QDateTime::currentMSecsSinceEpoch() - startedTime > MAX_DURATION) {
    stopRecording();
    startRecording();
    return;
  }

  if (rootWidget) {
    QImage img = rootWidget->grab().toImage();
    std::lock_guard<std::mutex> lock(imageMutex);
    latestImage = img;
  }
}

void ScreenRecorder::toggleRecording() {
  recording ? stopRecording() : startRecording();
}

void ScreenRecorder::startRecording() {
  recording = true;
  rootWidget = topWidget(this);
  startedTime = QDateTime::currentMSecsSinceEpoch();

  QString folderName = QDateTime::currentDateTime().toString("yyyy-MM-dd_hh:mm_AP");
  if (!recordingsFolder.exists(folderName)) {
    recordingsFolder.mkpath(folderName);
  }
  outputFolder = recordingsFolder.filePath(folderName);
  frameCounter = 0;

  encodingThread = std::thread(&ScreenRecorder::encodeImage, this);
}

void ScreenRecorder::stopRecording() {
  if (!recording) {
    return;
  }

  recording = false;

  if (encodingThread.joinable()) {
    encodingThread.join();
  }
}

void ScreenRecorder::encodeImage() {
  while (recording) {
    QImage image;
    {
      std::lock_guard<std::mutex> lock(imageMutex);
      image = latestImage;
    }

    if (!image.isNull()) {
      QImage convertedImage = image.convertToFormat(QImage::Format_RGBA8888);
      libyuv::ARGBScale(
        convertedImage.bits(),
        convertedImage.width() * 4,
        convertedImage.width(),
        convertedImage.height(),
        rgbScaleBuffer.data(),
        SCREEN_WIDTH * 4,
        SCREEN_WIDTH,
        SCREEN_HEIGHT,
        libyuv::kFilterBilinear
      );

      QImage scaledImage(rgbScaleBuffer.data(), SCREEN_WIDTH, SCREEN_HEIGHT, QImage::Format_RGBA8888);
      QString filename = QString("%1/frame_%2.png").arg(outputFolder).arg(frameCounter++, 5, 10, QChar('0'));
      scaledImage.copy().save(filename);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(1000 / UI_FREQ));
    std::this_thread::yield();
  }
}

void ScreenRecorder::paintEvent(QPaintEvent *event) {
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing);

  if (recording) {
    painter.setPen(QPen(redColor(), 6));
    painter.setBrush(redColor(166));
    painter.setFont(InterFont(25, QFont::Bold));
  } else {
    painter.setPen(QPen(redColor(), 6));
    painter.setBrush(blackColor(166));
    painter.setFont(InterFont(25, QFont::DemiBold));
  }

  int centeringOffset = 10;
  QRect buttonRect(centeringOffset, btn_size / 3, btn_size - centeringOffset * 2, btn_size / 3);
  painter.drawRoundedRect(buttonRect, 24, 24);

  QRect textRect = buttonRect.adjusted(centeringOffset, 0, -centeringOffset, 0);
  painter.setPen(QPen(whiteColor(), 6));
  painter.drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, tr("RECORD"));

  if (recording && ((QDateTime::currentMSecsSinceEpoch() - startedTime) / 1000) % 2 == 0) {
    painter.setPen(Qt::NoPen);
    painter.drawEllipse(QPoint(buttonRect.right() - btn_size / 10 - centeringOffset, buttonRect.center().y()), btn_size / 10, btn_size / 10);
  }
}
