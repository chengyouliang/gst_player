#if 1
#include <unistd.h>
#include <gst/gst.h>
#include <glib.h>

#include <QApplication>
#include <QTime>
#include <QWidget>
#include <gst/video/videooverlay.h>

#include "playerwindow.h"

#include "common.h"

extern "C"
{
#include <mediaPlayer.h>
}

ST_USER_HANDLE *userHandle;

void hanleCallBackEvent(CALL_BACK_EVENT_TYPE eventType, void *param);
void *_palyer_control_thread(void* Parameter);
gboolean video_widget_draw_cb (QWidget * widget,PlayerWindow *window);

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    int ret = 0;
    ST_INIT_PARAM initParam;
    WId xwinid;
    int maxWaitCount =10;
    QWidget *videowindow;

    /*load qss file*/
    QFile file(":/style.qss");

    /* 判断文件是否存在 */
    if (file.exists() ) {
        LOG_INFO("find qss file\n");
        /* 以只读的方式打开 */
        file.open(QFile::ReadOnly);
        /* 以字符串的方式保存读出的结果 */
        QString styleSheet = QLatin1String(file.readAll());
        /* 设置全局样式 */
        qApp->setStyleSheet(styleSheet);
        /* 关闭文件 */
        file.close();
    }
    else
    {
        LOG_INFO("not find qss file\n");
    }


    userHandle = (ST_USER_HANDLE *)malloc(sizeof(ST_USER_HANDLE));
    if (!userHandle)
    {
        LOG_ERROR ("malloc userHandle fail.\n");
        ret = -1;
    }
    LOG_INFO("malloc MEM(%p)\n", userHandle);
    memset(userHandle, 0, sizeof(ST_USER_HANDLE));

    initParam.path = "file:///mnt/media/rk3399_linux_release_v2.5.1_20210301/buildroot/output/rockchip_rk3399/build/gst_player-release/myVideo/123.mkv";
    
    //initParam.path = "https://www.freedesktop.org/software/gstreamer-sdk/data/media/sintel_cropped_multilingual.webm";
    initParam.callBackFunction = hanleCallBackEvent;
    initParam.logLevel = LOG_LEVEL_TRACE; 
    ret = MMPlayerInit(&initParam);
    if (ret != 0)
    {
        LOG_ERROR ("play file %s error.\n", initParam.path);
        return - 1;
    }
    userHandle->handleStatus = INIT_STATUS;

    //wait player init OK
    while (NULL ==  userHandle->pipeline)
    {
        maxWaitCount++;
        if (maxWaitCount > 2000)
        {
            LOG_ERROR ("wait palyer init OK msg fail.\n", initParam.path);
            return - 1;
        }
        usleep(1000);
    }

    LOG_INFO("recive palyer init OK msg\n");
    maxWaitCount = 0;

    //play mm in QT window
    PlayerWindow *window = new PlayerWindow(&userHandle);
    window->setWindowTitle("Qt&&GStreamer Player demo");
    window->resize(1280, 720);
    window->move(0,0);
    xwinid = window->getVideoWId();
    //wait playsink added
    while (NULL == userHandle->videoSink)
    {
        userHandle->videoSink = gst_bin_get_by_name (GST_BIN (userHandle->pipeline), "playsink");
        maxWaitCount++;
        if (maxWaitCount > 2000)
        {
            LOG_ERROR ("can not find video sink.\n", initParam.path);
            goto stop;
        }
        usleep(1000);
    }

    gst_video_overlay_set_window_handle (GST_VIDEO_OVERLAY(userHandle->videoSink), xwinid);
    if (userHandle->videoSink)
    {
         gst_object_unref (userHandle->videoSink);
    }

    usleep(1);
    videowindow = window->getVideo();
    //QPoint p5 = videowindow->mapToGlobal(QPoint(0,0));
    gst_video_overlay_set_render_rectangle (GST_VIDEO_OVERLAY (userHandle->videoSink),videowindow->pos().x(),videowindow->pos().y(),1280,720);
    LOG_INFO("%s %d  %d %d %d %d\n",__FUNCTION__,__LINE__,videowindow->pos().x(),videowindow->pos().y(),videowindow->width(),videowindow->height());
    //g_signal_connect (window->getVideo(), "draw",G_CALLBACK (video_widget_draw_cb), window);
    window->show();

    //init cmd queue
    g_mutex_init(&userHandle->quitMutex);
    g_cond_init(&userHandle->quitCond);
    g_mutex_init(&userHandle->queueMutex);
    g_cond_init(&userHandle->queueCond);
    cmdQueueInit(&userHandle->cmdQueue);
    userHandle->controlThread = g_thread_new("control_thread", _palyer_control_thread, userHandle);

    if (!userHandle->controlThread)
    {
        LOG_ERROR ("create control thread fail.\n");
    }

stop:
    return app.exec();
}

/* We use the "draw" callback to change the size of the sink
 * because the "configure-event" is only sent to top-level widgets. */
gboolean video_widget_draw_cb (QWidget * widget,PlayerWindow *window)
{
LOG_INFO("%s %d\n",__FUNCTION__,__LINE__);
  gst_video_overlay_set_render_rectangle (GST_VIDEO_OVERLAY (userHandle->videoSink),widget->pos().x(),widget->pos().y(),widget->width(), widget->height());
  /* There is no need to call gst_video_overlay_expose().
   * The wayland compositor can always re-draw the window
   * based on its last contents if necessary */

  return FALSE;
}

void hanleCallBackEvent(CALL_BACK_EVENT_TYPE eventType, void *param)
{
    LOG_INFO("recive event type (%d)\n", eventType);
    switch (eventType)
    {
        case PLAYER_INIT_OK:
        {
            LOG_INFO("player init OK! \n");
            ST_HANDLE_INFO *retHanleInfo = (ST_HANDLE_INFO *)param;
            userHandle->handleId = retHanleInfo->handleId;
            userHandle->pipeline = retHanleInfo->pipeline;
            userHandle->handleStatus = READY_STATUS;
            break;
        }
        case PLAYER_STOP_OK:
        {
            LOG_INFO("pipeline deinit OK! \n");
            userHandle->handleStatus = ERROR_STATUS;
            g_mutex_lock(&userHandle->queueMutex);
            g_cond_signal(&userHandle->queueCond);
            g_mutex_unlock(&userHandle->queueMutex);
            break;
        }

        default:
            break;
    }
}

void *_palyer_control_thread(void* Parameter)
{
    ST_USER_HANDLE *pstUserHandle = (ST_USER_HANDLE *)Parameter;
    ST_PLAYER_CMD *curCmd = NULL;

    if (NULL == pstUserHandle)
    {
        LOG_ERROR ("handle is NULL.\n");
    }

    LOG_INFO("control thread enter\n");
    while(pstUserHandle->handleStatus < ERROR_STATUS)
    {
        while (!cmdQueueIsEmpty(pstUserHandle->cmdQueue))
        {
            LOG_INFO("queue not empty\n");
            g_mutex_lock(&pstUserHandle->queueMutex);
            curCmd = cmdQueuePop(pstUserHandle->cmdQueue);
            g_mutex_unlock(&pstUserHandle->queueMutex);

            if (curCmd == NULL)
            {
                LOG_INFO("cmd is empty\n");
                break;
            }

            switch (curCmd->type)
            {
                case CMD_PLAY:
                {
                    MMPlayerPlay(pstUserHandle->handleId);
                    pstUserHandle->handleStatus = PLAYING_STATUS;
                    break;
                }
                case CMD_PAUSE:
                {
                    MMPlayerPause(pstUserHandle->handleId);
                    pstUserHandle->handleStatus = PAUSE_STATUS;
                    break;
                }
                case CMD_RESUME:
                {
                    break;
                }
                case CMD_STOP:
                {
                    MMPlayerStop(pstUserHandle->handleId);
                    break;
                }
                case CMD_SEEK:
                {
                    break;
                }
                default:
                    break;
            }

            //need free cmd here
            if (curCmd)
            {
                g_free(curCmd);
            }
        }

        if (cmdQueueIsEmpty(pstUserHandle->cmdQueue))
        {
            //wait new cmd push in cmd queue
            LOG_INFO("start wait new cmd...\n");
            g_mutex_lock(&pstUserHandle->queueMutex);
            g_cond_wait(&pstUserHandle->queueCond, &pstUserHandle->queueMutex);
            g_mutex_unlock(&pstUserHandle->queueMutex);
            LOG_INFO("wait new cmd done\n");
        }
    }

    LOG_INFO("quit control thread\n");
    cmdQueueDeInit(pstUserHandle->cmdQueue);
    //control thead stop free cmd queue
    if (pstUserHandle->cmdQueue)
    {
        g_free(pstUserHandle->cmdQueue);
        pstUserHandle->cmdQueue = NULL;
    }

    g_mutex_clear(&pstUserHandle->queueMutex);
    g_cond_clear(&pstUserHandle->queueCond);
    g_thread_unref(pstUserHandle->controlThread);
    pstUserHandle->controlThread = NULL;

    //emit quit signal
    g_mutex_lock(&userHandle->quitMutex);
    g_cond_signal(&userHandle->quitCond);
    g_mutex_unlock(&userHandle->quitMutex);

}

#else
 #include <glib.h>;
 #include <gst/gst.h>;
 #include <gst/video/videooverlay.h>;
 #include <QApplication>;
 #include <QTimer>;
 #include <QWidget>;

 int main(int argc, char *argv[])
 {
   if (!g_thread_supported ())
     g_thread_init (NULL);

   gst_init (&argc, &argv);
   QApplication app(argc, argv);
   app.connect(&app, SIGNAL(lastWindowClosed()), &app, SLOT(quit ()));

   // prepare the pipeline

   GstElement *pipeline = gst_pipeline_new ("xvoverlay");
   GstElement *src = gst_element_factory_make ("videotestsrc", NULL);
   GstElement *sink = gst_element_factory_make ("waylandsink", NULL);
   gst_bin_add_many (GST_BIN (pipeline), src, sink, NULL);
   gst_element_link (src, sink);

   // prepare the ui

   QWidget window;
   window.setGeometry(0, 0, 320, 240); // (x, y, w, h)
   //window.resize(320, 240);
   window.show();

   WId xwinid = window.winId();
   gst_video_overlay_set_window_handle (GST_VIDEO_OVERLAY (sink), xwinid);
   QPoint p5 = window.mapToGlobal(QPoint(0, 0));
   gst_video_overlay_set_render_rectangle (GST_VIDEO_OVERLAY (sink),p5.x(),
       p5.y(), window.width(), window.height());

   // run the pipeline

   GstStateChangeReturn sret = gst_element_set_state (pipeline,
       GST_STATE_PLAYING);
   if (sret == GST_STATE_CHANGE_FAILURE) {
     gst_element_set_state (pipeline, GST_STATE_NULL);
     gst_object_unref (pipeline);
     // Exit application
     QTimer::singleShot(0, QApplication::activeWindow(), SLOT(quit()));
   }

   int ret = app.exec();

   window.hide();
   gst_element_set_state (pipeline, GST_STATE_NULL);
   gst_object_unref (pipeline);

   return ret;
 }
#endif
