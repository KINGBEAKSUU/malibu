#include "libyuv.h"

#include "selfdrive/ui/qt/util.h"

#include "selfdrive/frogpilot/screenrecorder/screenrecorder.h"

ScreenRecorder::ScreenRecorder(QWidget *parent) : QPushButton(parent) {
  setFixedSize(btn_size, btn_size);

  encoder = std::make_unique<OmxEncoder>("/data/media/screen_recordings", screenWidth, screenHeight, UI_FREQ, 8 * 1024 * 1024);

  rgbScaleBuffer.resize(screenWidth * screenHeight * 4);

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

  if (QDateTime::currentMSecsSinceEpoch() - startedTime > recordingDurationLimit) {
    stopRecording();
    startRecording();
    return;
  }

  if (rootWidget) {
    imageQueue.push(rootWidget->grab().toImage());
  }
}

void ScreenRecorder::toggleRecording() {
  recording ? stopRecording() : startRecording();
}

void ScreenRecorder::startRecording() {
  encoder->encoder_open((QDateTime::currentDateTime().toString("yyyy-MM-dd_hh:mm_AP").toStdString() + ".mp4").c_str());

  recording = true;
  rootWidget = topWidget(this);

  startedTime = QDateTime::currentMSecsSinceEpoch();

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

  encoder->encoder_close();
}

void ScreenRecorder::encodeImage() {
  uint64_t startTime = nanos_since_boot();

  while (recording) {
    QImage image;
    if (imageQueue.pop_wait_for(image, std::chrono::milliseconds(1000 / UI_FREQ))) {
      QImage convertedImage = image.convertToFormat(QImage::Format_RGBA8888);
      libyuv::ARGBScale(
        convertedImage.bits(),
        convertedImage.width() * 4,
        convertedImage.width(),
        convertedImage.height(),
        rgbScaleBuffer.data(),
        screenWidth * 4,
        screenWidth,
        screenHeight,
        libyuv::kFilterBilinear
      );
      encoder->encode_frame_rgba(
        rgbScaleBuffer.data(),
        screenWidth,
        screenHeight,
        nanos_since_boot() - startTime
      );
    }

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
