/*
 * Copyright (C) 2013-2014 Canonical, Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; version 3.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "aalcameraservice.h"
#include "aalimagecapturecontrol.h"
#include "aalimageencodercontrol.h"
#include "aalmetadatawritercontrol.h"
#include "aalvideorenderercontrol.h"
#include "storagemanager.h"

#include <hybris/camera/camera_compatibility_layer.h>
#include <hybris/camera/camera_compatibility_layer_capabilities.h>

#include <QDir>
#include <QObject>
#include <QFile>
#include <QFileInfo>
#include <QMediaPlayer>
#include <QStandardPaths>
#include <QDateTime>
#include <QGuiApplication>
#include <QScreen>
#include <QSettings>
#include <QtConcurrent/QtConcurrent>

AalImageCaptureControl::AalImageCaptureControl(AalCameraService *service, QObject *parent)
   : QCameraImageCaptureControl(parent),
    m_service(service),
    m_cameraControl(service->cameraControl()),
    m_lastRequestId(0),
    m_ready(false),
    m_targetFileName(),
    m_pendingCaptureFile(),
    m_captureCancelled(false),
    m_screenAspectRatio(0.0),
    m_audioPlayer(new QMediaPlayer(this))
{
    m_galleryPath = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    m_audioPlayer->setMedia(QUrl::fromLocalFile("/system/media/audio/ui/camera_click.ogg"));
#if QT_VERSION < QT_VERSION_CHECK(5, 5, 0)
    m_audioPlayer->setAudioRole(QMediaPlayer::AlertRole);
#else
    m_audioPlayer->setAudioRole(QAudio::NotificationRole);
#endif
}

AalImageCaptureControl::~AalImageCaptureControl()
{
    delete(m_audioPlayer);
}

bool AalImageCaptureControl::isReadyForCapture() const
{
    return m_ready;
}

int AalImageCaptureControl::capture(const QString &fileName)
{
    m_lastRequestId++;
    if (!m_ready || !m_service->androidControl()) {
        emit error(m_lastRequestId, QCameraImageCapture::NotReadyError,
                   QLatin1String("Camera not ready to capture"));
        return m_lastRequestId;
    }

    m_targetFileName = fileName;
    m_captureCancelled = false;

    android_camera_take_snapshot(m_service->androidControl());

    m_service->updateCaptureReady();

    m_service->videoOutputControl()->createPreview();

    return m_lastRequestId;
}

void AalImageCaptureControl::cancelCapture()
{
    m_captureCancelled = true;
    m_pendingCaptureFile.clear();
}

void AalImageCaptureControl::shutterCB(void *context)
{
    Q_UNUSED(context);
    QMetaObject::invokeMethod(AalCameraService::instance()->imageCaptureControl(),
                              "shutter", Qt::QueuedConnection);
}

void AalImageCaptureControl::saveJpegCB(void *data, uint32_t data_size, void *context)
{
    Q_UNUSED(context);

    // Copy the data buffer so that it is safe to pass it off to another thread,
    // since it will be destroyed once this function returns
    QByteArray dataCopy((const char*)data, data_size);

    QMetaObject::invokeMethod(AalCameraService::instance()->imageCaptureControl(),
                              "saveJpeg", Qt::QueuedConnection,
                              Q_ARG(QByteArray, dataCopy));
}

void AalImageCaptureControl::init(CameraControl *control, CameraControlListener *listener)
{
    Q_UNUSED(control);

    listener->on_msg_shutter_cb = &AalImageCaptureControl::shutterCB;
    listener->on_data_compressed_image_cb = &AalImageCaptureControl::saveJpegCB;

    connect(m_service->videoOutputControl(), SIGNAL(previewReady()), this, SLOT(onPreviewReady()));
}

void AalImageCaptureControl::onPreviewReady()
{
    // The preview image was fully captured, notify the UI layer
    Q_EMIT imageCaptured(m_lastRequestId, m_service->videoOutputControl()->preview());
}

void AalImageCaptureControl::setReady(bool ready)
{
    if (m_ready != ready) {
        m_ready = ready;
        Q_EMIT readyForCaptureChanged(m_ready);
    }
}

bool AalImageCaptureControl::isCaptureRunning() const
{
    return !m_pendingCaptureFile.isNull();
}

void AalImageCaptureControl::shutter()
{
    bool playShutterSound = m_settings.value("playShutterSound", true).toBool();
    if (playShutterSound) {
        m_audioPlayer->play();
    }
    Q_EMIT imageExposed(m_lastRequestId);
}

void AalImageCaptureControl::saveJpeg(const QByteArray& data)
{
    if (m_captureCancelled) {
        m_captureCancelled = false;
        return;
    }

    // Copy the metadata so that we can clear its container
    QVariantMap metadata;
    AalMetaDataWriterControl* metadataControl = m_service->metadataWriterControl();
    Q_FOREACH(QString key, metadataControl->availableMetaData()) {
        metadata.insert(key, metadataControl->metaData(key));
    }
    metadata.insert("CorrectedOrientation", metadataControl->correctedOrientation());
    m_service->metadataWriterControl()->clearAllMetaData();
    m_pendingCaptureFile.clear();

    QString fileName = m_targetFileName;
    m_targetFileName.clear();

    // Restart the viewfinder and notify that the camera is ready to capture again
    if (m_service->androidControl()) {
        android_camera_start_preview(m_service->androidControl());
    }
    m_service->updateCaptureReady();

    StringFutureWatcher* watcher = new StringFutureWatcher;
    QObject::connect(watcher, &QFutureWatcher<QString>::finished, this, &AalImageCaptureControl::onImageFileSaved);
    m_pendingSaveOperations.insert(watcher, m_lastRequestId);

    QFuture<QString> future = QtConcurrent::run(m_storageManager, &StorageManager::saveJpegImage,
                                                data, metadata, fileName);
    watcher->setFuture(future);

}

void AalImageCaptureControl::onImageFileSaved()
{
    StringFutureWatcher* watcher = static_cast<StringFutureWatcher*>(sender());

    if (m_pendingSaveOperations.contains(watcher)) {
        int requestID = m_pendingSaveOperations.take(watcher);

        QString fileName = watcher->result();
        delete watcher;

        if (!fileName.isEmpty()) {
            Q_EMIT imageSaved(requestID, fileName);
        } else {
            // emit error as empty file name means the save failed
            // FIXME: better way to report errors
        }
    }
}
