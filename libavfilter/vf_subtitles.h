#include "avfilter.h"
#include "libavcodec/avcodec.h"
#include <ass/ass.h>
#include "drawutils.h"
#include "fftools/ffmpeg.h"

int append_subtitle_packet (InputStream *ist, AVPacket *pkt, AVFrame *frame);