#pragma once

#include <mutex>

#include "selfdrive/ui/qt/onroad/buttons.h"

class ScreenRecorder : public QPushButton {
  Q_OBJECT

public:
  explicit ScreenRecorder(QWidget *parent = nullptr);
  ~ScreenRecorder() override;

protected:
  void paintEvent(QPaintEvent *event) override;

private slots:
  void toggleRecording();

private:
  void encodeImage();
  void startRecording();
  void stopRecording();
  void updateState();

  bool recording;

  int frameCounter;

  qint64 startedTime;

  std::mutex imageMutex;

  std::thread encodingThread;

  std::vector<uint8_t> rgbScaleBuffer;

  QColor blackColor(int alpha = 255) { return QColor(0, 0, 0, alpha); }
  QColor redColor(int alpha = 255) { return QColor(201, 34, 49, alpha); }
  QColor whiteColor(int alpha = 255) { return QColor(255, 255, 255, alpha); }

  QDir recordingsFolder;

  QImage latestImage;

  QString outputFolder;

  QWidget *rootWidget;
};
