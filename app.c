/*
 * Copyright (c) 2010, Texas Instruments Incorporated
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * *  Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * *  Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * *  Neither the name of Texas Instruments Incorporated nor the names of
 *    its contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
 * EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */
/*
 *  ======== app.c ========
 */
#include <xdc/std.h>
#include <ti/sdo/ce/Engine.h>
#include <ti/sdo/ce/osal/Memory.h>
#include <ti/sdo/ce/video/videnc.h>
#include <ti/sdo/ce/video/viddec.h>
#include <ti/sdo/ce/trace/gt.h>

#include <string.h>  /* for memset */

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

#include <fcntl.h>              /* low-level i/o */
#include <unistd.h>
#include <errno.h>
#include <malloc.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <sys/ioctl.h>

#include <asm/types.h>          /* for videodev2.h */

#include <linux/videodev2.h>

#define IMG_HEIGHT          480
#define IMG_WIDTH           640

#define CLEAR(x)            memset (&(x), 0, sizeof (x))



/* Not a cached system, no buffer alignment constraints */
#define BUFALIGN            Memory_DEFAULTALIGNMENT



#define NSAMPLES    1024  /* must be multiple of 128 for cache/DMA reasons */
// #define IFRAMESIZE  (NSAMPLES * sizeof(Int8))  /* raw frame (input) */
// #define EFRAMESIZE  (NSAMPLES * sizeof(Int8))  /* encoded frame */
// #define OFRAMESIZE  (NSAMPLES * sizeof(Int8))  /* decoded frame (output) */

#define IFRAMESIZE  (NSAMPLES * 600 * sizeof(Int8))  /* raw frame (input) */
#define EFRAMESIZE  (NSAMPLES * 600 * sizeof(Int8))  /* encoded frame */
#define OFRAMESIZE  (NSAMPLES * 300 * sizeof(Int8))  /* decoded frame (output) */

static XDAS_Int8 *inBuf;
static XDAS_Int8 *encodedBuf;
static XDAS_Int8 *outBuf;

static String progName     = "app";
static String decoderName  = "viddec_copy";
static String encoderName  = "videnc_copy";
static String engineName   = "video_copy";


static String usage = "%s: dev_name input-file output-file\n";

static Void encode_decode(VIDENC_Handle enc, VIDDEC_Handle dec, FILE *in,
    FILE *out);

extern GT_Mask curMask;


//摄像机采集模式
typedef enum {
    IO_METHOD_READ, IO_METHOD_MMAP, IO_METHOD_USERPTR,
} io_method;

static io_method io = IO_METHOD_MMAP;

struct buffer {
    void * start;
    size_t length;
};

static struct buffer *buffers = NULL;
static unsigned int n_buffers = 0;

static String dev_name = "/dev/video0";
static int fd = -1;

FILE *in = NULL;
FILE *out = NULL;

static unsigned char* grayBuf = NULL;

// 错误处理函数
static void errno_exit(const char * s)
{

    fprintf(stderr, "%s error %d, %s/n", s, errno, strerror(errno));

    exit(EXIT_FAILURE);
}


// ioctl函数
static int xioctl(int fd, int request, void * arg)
{

    int r;

    do {
        r = ioctl(fd, request, arg);
    } while (-1 == r && EINTR == errno);

    return r;
}

static int yuv422_to_gray(unsigned char* yuv422, unsigned char *gray, 
    unsigned int width, unsigned int height)
{

    unsigned int ynum = width * height;
    int i;

    for(i = 0; i < ynum; i++)
    {
       //printf("yuv422_to_yuv420  Y start, %d\n", i);
       gray[i] = yuv422[i * 2];
    }

   return 0;
}


// 处理函数
static void process_image(const void * p, int size) {

    memset(grayBuf, 0 , OFRAMESIZE);
    yuv422_to_gray((unsigned char*)p, grayBuf, IMG_WIDTH, IMG_HEIGHT);
    fwrite(grayBuf, size, 1, in);

}

// 读取一帧图像
static int read_frame(void) {

    struct v4l2_buffer buf;
    unsigned int i;

    CLEAR(buf);

    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;

    if (-1 == xioctl(fd, VIDIOC_DQBUF, &buf)) {
        switch (errno) {
        case EAGAIN:
            return 0;

        case EIO:
            /* Could ignore EIO, see spec. */

            /* fall through */

        default:
            errno_exit("VIDIOC_DQBUF");
        }
    }

    assert(buf.index < n_buffers);

    process_image(buffers[buf.index].start, buffers[buf.index].length);

    if (-1 == xioctl(fd, VIDIOC_QBUF, &buf))
        errno_exit("VIDIOC_QBUF");

    return 1;
}


static void mainloop(void) {

    unsigned int count;

    count = 100;

    while (count-- > 0) {
        for (;;) {

            fd_set fds;
            struct timeval tv;
            int r;

            FD_ZERO(&fds);
            FD_SET(fd, &fds);

            /* Timeout. */
            tv.tv_sec = 3;
            tv.tv_usec = 0;

            r = select(fd + 1, &fds, NULL, NULL, &tv);

            if (-1 == r) {
                if (EINTR == errno)
                    continue;

                errno_exit("select");
            }

            if (0 == r) {
                fprintf(stderr, "select timeout/n");
                exit(EXIT_FAILURE);
            }

            if (read_frame())
                break;

            /* EAGAIN - continue select loop. */
        }
    }
}


static void start_capturing(void) {

    unsigned int i;
    enum v4l2_buf_type type;


    for (i = 0; i < n_buffers; ++i) {

        struct v4l2_buffer buf;

        CLEAR(buf);

        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;

        if (-1 == xioctl(fd, VIDIOC_QBUF, &buf)) {//将拍摄的图像数据放入缓存队列
            errno_exit("VIDIOC_QBUF");
        }

    }

    type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    if (-1 == xioctl(fd, VIDIOC_STREAMON, &type)) {

        errno_exit("VIDIOC_STREAMON");
    }

    // printf("start capture finish\n");
}


static void init_mmap(void) {

    struct v4l2_requestbuffers req;  //缓冲区结构体

    CLEAR(req);

    req.count = 4;  //缓存数量，设置缓存区队列里保持多少张照片
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;  //方法为内存映射方法

    if (-1 == xioctl(fd, VIDIOC_REQBUFS, &req)) {
        if (EINVAL == errno) {
            fprintf(stderr, "%s does not support "
                    "memory mapping/n", dev_name);
            exit(EXIT_FAILURE);
        } else {
            errno_exit("VIDIOC_REQBUFS");
        }
    }

    if (req.count < 2) {
        fprintf(stderr, "Insufficient buffer memory on %s/n", dev_name);
        exit(EXIT_FAILURE);
    }

    buffers = (struct buffer *)calloc(req.count, sizeof(struct buffer));

    if (!buffers) {

        fprintf(stderr, "Out of memory/n");
        exit(EXIT_FAILURE);
    }

    for (n_buffers = 0; n_buffers < req.count; ++n_buffers) {

        struct v4l2_buffer buf;

        CLEAR(buf);

        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = n_buffers;

        if (-1 == xioctl(fd, VIDIOC_QUERYBUF, &buf)) {
            errno_exit("VIDIOC_QUERYBUF");
        }


        buffers[n_buffers].length = buf.length;
        buffers[n_buffers].start = mmap(NULL /* start anywhere */, buf.length,
                PROT_READ | PROT_WRITE /* required */,
                MAP_SHARED /* recommended */, fd, buf.m.offset);  //缓冲区起始存储位置

        if (MAP_FAILED == buffers[n_buffers].start){
            errno_exit("mmap");
        }

    }
}


static void init_device(void) {

    struct v4l2_capability cap;

    //query device capability
    if (-1 == xioctl(fd, VIDIOC_QUERYCAP, &cap)) {
        if (EINVAL == errno) {
            fprintf(stderr, "%s is no V4L2 device/n", dev_name);
            exit(EXIT_FAILURE);
        } else {
            errno_exit("VIDIOC_QUERYCAP");
        }
    }

    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
        fprintf(stderr, "%s is no video capture device/n", dev_name);
        exit(EXIT_FAILURE);
    }


    if (!(cap.capabilities & V4L2_CAP_STREAMING)) { //是否支持流操作，支持数据流控制
        fprintf(stderr, "%s does not support streaming i/o/n", dev_name);
        exit(EXIT_FAILURE);
    }


    /* Select video input, video standard and tune here. */
    struct v4l2_cropcap cropcap;
    struct v4l2_crop crop;

    CLEAR(cropcap);

    cropcap.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;  //表示支持图像获取

    if (0 == xioctl (fd, VIDIOC_CROPCAP, &cropcap)) {
        crop.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        crop.c = cropcap.defrect; /* reset to default */

        if (-1 == xioctl (fd, VIDIOC_S_CROP, &crop)) {
                switch (errno) {
                case EINVAL:
                        /* Cropping not supported. */
                        break;
                default:
                        /* Errors ignored. */
                        break;
                }
        }
    } else {
        /* Errors ignored. */
    }

    struct v4l2_format fmt;
    unsigned int min;

    CLEAR(fmt);

    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = IMG_WIDTH;
    fmt.fmt.pix.height = IMG_HEIGHT;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;

    if (-1 == xioctl(fd, VIDIOC_S_FMT, &fmt))
    {
        errno_exit("VIDIOC_S_FMT");
    }


    /* Note VIDIOC_S_FMT may change width and height. */

    /* Buggy driver paranoia. */
    min = fmt.fmt.pix.width * 2;
    if (fmt.fmt.pix.bytesperline < min)
    {
        fmt.fmt.pix.bytesperline = min;
    }

    min = fmt.fmt.pix.bytesperline * fmt.fmt.pix.height;
    if (fmt.fmt.pix.sizeimage < min)
    {
        fmt.fmt.pix.sizeimage = min;
    }

    init_mmap();

    // printf("init device finish\n");
}



static void open_device(void) {

    struct stat st;

    if (-1 == stat(dev_name, &st)) {
        fprintf(stderr, "Cannot identify '%s': %d, %s/n", dev_name, errno,
                strerror(errno));
        exit(EXIT_FAILURE);
    }

    if (!S_ISCHR(st.st_mode)) {
        fprintf(stderr, "%s is no device/n", dev_name);
        exit(EXIT_FAILURE);
    }

    fd = open(dev_name, O_RDWR /* required */| O_NONBLOCK, 0);

    if (-1 == fd) {
        fprintf(stderr, "Cannot open '%s': %d, %s/n", dev_name, errno,
                strerror(errno));
        exit(EXIT_FAILURE);
    }

    // printf("open device finish\n");

}

static void stop_capturing(void) {

    enum v4l2_buf_type type;

    type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    if (-1 == xioctl(fd, VIDIOC_STREAMOFF, &type))
        errno_exit("VIDIOC_STREAMOFF");

}


static void uninit_device(void) {

    unsigned int i;

    for (i = 0; i < n_buffers; ++i)
        if (-1 == munmap(buffers[i].start, buffers[i].length))
            errno_exit("munmap");


    free(buffers);
}

static void close_device(void) {

    if (-1 == close(fd))
        errno_exit("close");

    fd = -1;
}


/*
 *  ======== createInFileIfMissing ========
 */
static void createInFileIfMissing( char *inFileName )
{

    int i;
    FILE *f = fopen(inFileName, "rb");
    if (f == NULL) {
        printf( "Input file '%s' not found, generating one.\n", inFileName );
        f = fopen( inFileName, "wb" );
        for (i = 0; i < 1024; i++) {
            fwrite(&i, sizeof(i), 1, f);
        }
    }

    fclose( f );
}

/*
 *  ======== smain ========
 */
Int smain(Int argc, String argv[])
{

    Engine_Handle ce = NULL;
    VIDDEC_Handle dec = NULL;
    VIDENC_Handle enc = NULL;


    String inFile, outFile;

    Memory_AllocParams allocParams;

    if (argc <= 1) {
        inFile = "./in.dat";
        outFile = "./out.dat";
        createInFileIfMissing(inFile);
    }
    else if (argc == 4) {
        progName = argv[0];
        dev_name = argv[1];
        inFile = argv[2];
        outFile = argv[3];
    }
    else {
        fprintf(stderr, usage, argv[0]);
        exit(1);
    }



    GT_0trace(curMask, GT_1CLASS, "App-> Application started.\n");

    /* allocate input, encoded, and output buffers */
    allocParams.type = Memory_CONTIGPOOL;
    allocParams.flags = Memory_NONCACHED;
    allocParams.align = BUFALIGN;
    allocParams.seg = 0;

    inBuf = (XDAS_Int8 *)Memory_alloc(IFRAMESIZE, &allocParams);
    encodedBuf = (XDAS_Int8 *)Memory_alloc(EFRAMESIZE, &allocParams);
    outBuf = (XDAS_Int8 *)Memory_alloc(OFRAMESIZE, &allocParams);

    grayBuf =  (unsigned char*)Memory_alloc(OFRAMESIZE, &allocParams);

    if ((inBuf == NULL) || (encodedBuf == NULL) || 
        (outBuf == NULL) || (grayBuf == NULL)) {
        goto end;
    }

    /* open file streams for input and output */
    if ((in = fopen(inFile, "rb")) == NULL) {
        printf("App-> ERROR: can't read file %s\n", inFile);
        goto end;
    }

    if ((out = fopen(outFile, "wb")) == NULL) {
        printf("App-> ERROR: can't write to file %s\n", outFile);
        goto end;
    }

    GT_0trace(curMask, GT_1CLASS, "Video -> Video capture started.\n");

    open_device();
    init_device();
    start_capturing();
    mainloop();

    stop_capturing();
    uninit_device();
    close_device();

    GT_0trace(curMask, GT_1CLASS, "Video -> Video capture done.\n");


    /* reset, load, and start DSP Engine */
    if ((ce = Engine_open(engineName, NULL, NULL)) == NULL) {
        fprintf(stderr, "%s: error: can't open engine %s\n",
            progName, engineName);
        goto end;
    }

    /* allocate and initialize video decoder on the engine */
    dec = VIDDEC_create(ce, decoderName, NULL);
    if (dec == NULL) {
        printf( "App-> ERROR: can't open codec %s\n", decoderName);
        goto end;
    }

    /* allocate and initialize video encoder on the engine */
    enc = VIDENC_create(ce, encoderName, NULL);
    if (enc == NULL) {
        fprintf(stderr, "%s: error: can't open codec %s\n",
            progName, encoderName);
        goto end;
    }

    /* use engine to encode, then decode the data */
    encode_decode(enc, dec, in, out);

    goto end;

end:
    /* teardown the codecs */
    if (enc) {
        VIDENC_delete(enc);
    }

    if (dec) {
        VIDDEC_delete(dec);
    }

    /* close the engine */
    if (ce) {
        Engine_close(ce);
    }

    /* close the files */
    if (in) {
        fclose(in);
    }

    if (out) {
        fclose(out);
    }

    /* free buffers */
    if (inBuf) {
        Memory_free(inBuf, IFRAMESIZE, &allocParams);
    }

    if (encodedBuf) {
        Memory_free(encodedBuf, EFRAMESIZE, &allocParams);
    }

    if (outBuf) {
        Memory_free(outBuf, OFRAMESIZE, &allocParams);
    }

    if (grayBuf) {
        Memory_free(grayBuf, OFRAMESIZE, &allocParams);
    }

    GT_0trace(curMask, GT_1CLASS, "app done.\n");
    return (0);
}

/*
 *  ======== encode_decode ========
 */
static Void encode_decode(VIDENC_Handle enc, VIDDEC_Handle dec, FILE *in,
    FILE *out)
{

    Int                         n;
    Int32                       status;

    VIDDEC_InArgs               decInArgs;
    VIDDEC_OutArgs              decOutArgs;
    VIDDEC_DynamicParams        decDynParams;
    VIDDEC_Status               decStatus;

    VIDENC_InArgs               encInArgs;
    VIDENC_OutArgs              encOutArgs;
    VIDENC_DynamicParams        encDynParams;
    VIDENC_Status               encStatus;

    XDM_BufDesc                 inBufDesc;
    XDAS_Int8                  *src[XDM_MAX_IO_BUFFERS];
    XDAS_Int32                  inBufSizes[XDM_MAX_IO_BUFFERS];

    XDM_BufDesc                 encodedBufDesc;
    XDAS_Int8                  *encoded[XDM_MAX_IO_BUFFERS];
    XDAS_Int32                  encBufSizes[XDM_MAX_IO_BUFFERS];

    XDM_BufDesc                 outBufDesc;
    XDAS_Int8                  *dst[XDM_MAX_IO_BUFFERS];
    XDAS_Int32                  outBufSizes[XDM_MAX_IO_BUFFERS];

    /* clear and initialize the buffer descriptors */
    memset(src,     0, sizeof(src[0])     * XDM_MAX_IO_BUFFERS);
    memset(encoded, 0, sizeof(encoded[0]) * XDM_MAX_IO_BUFFERS);
    memset(dst,     0, sizeof(dst[0])     * XDM_MAX_IO_BUFFERS);

    src[0]     = inBuf;
    encoded[0] = encodedBuf;
    dst[0]     = outBuf;

    inBufDesc.numBufs = encodedBufDesc.numBufs = outBufDesc.numBufs = 1;

    inBufDesc.bufSizes      = inBufSizes;
    encodedBufDesc.bufSizes = encBufSizes;
    outBufDesc.bufSizes     = outBufSizes;

    inBufSizes[0] = encBufSizes[0] = outBufSizes[0] = NSAMPLES;

    inBufDesc.bufs      = src;
    encodedBufDesc.bufs = encoded;
    outBufDesc.bufs     = dst;

    /* initialize all "sized" fields */
    encInArgs.size    = sizeof(encInArgs);
    decInArgs.size    = sizeof(decInArgs);
    encOutArgs.size   = sizeof(encOutArgs);
    decOutArgs.size   = sizeof(decOutArgs);
    encDynParams.size = sizeof(encDynParams);
    decDynParams.size = sizeof(decDynParams);
    encStatus.size    = sizeof(encStatus);
    decStatus.size    = sizeof(decStatus);

    /*
     * Query the encoder and decoder.
     * This app expects the encoder to provide 1 buf in and get 1 buf out,
     * and the buf sizes of the in and out buffer must be able to handle
     * NSAMPLES bytes of data.
     */
    status = VIDENC_control(enc, XDM_GETSTATUS, &encDynParams, &encStatus);
    if (status != VIDENC_EOK) {
        /* failure, report error and exit */
        GT_1trace(curMask, GT_7CLASS, "encode control status = 0x%x\n", status);
        return;
    }

    /* Validate this encoder codec will meet our buffer requirements */
    if ((inBufDesc.numBufs < encStatus.bufInfo.minNumInBufs) ||
        (IFRAMESIZE < encStatus.bufInfo.minInBufSize[0]) ||
        (encodedBufDesc.numBufs < encStatus.bufInfo.minNumOutBufs) ||
        (EFRAMESIZE < encStatus.bufInfo.minOutBufSize[0])) {

        /* failure, report error and exit */
        GT_0trace(curMask, GT_7CLASS,
            "Error:  encoder codec feature conflict\n");
        return;
    }

    status = VIDDEC_control(dec, XDM_GETSTATUS, &decDynParams, &decStatus);
    if (status != VIDDEC_EOK) {
        /* failure, report error and exit */
        GT_1trace(curMask, GT_7CLASS, "decode control status = 0x%x\n", status);
        return;
    }

    /* Validate this decoder codec will meet our buffer requirements */
    if ((inBufDesc.numBufs < decStatus.bufInfo.minNumInBufs) ||
        (EFRAMESIZE < decStatus.bufInfo.minInBufSize[0]) ||
        (outBufDesc.numBufs < decStatus.bufInfo.minNumOutBufs) ||
        (OFRAMESIZE < decStatus.bufInfo.minOutBufSize[0])) {

        /* failure, report error and exit */
        GT_0trace(curMask, GT_7CLASS,
            "App-> ERROR: decoder does not meet buffer requirements.\n");
        return;
    }

    /*
     * Read complete frames from in, encode, decode, and write to out.
     */
    for (n = 0; fread(inBuf, IFRAMESIZE, 1, in) == 1; n++) {

        /* decode the frame */
        status = VIDDEC_process(dec, &inBufDesc, &outBufDesc, &decInArgs,
           &decOutArgs);

        // GT_2trace(curMask, GT_2CLASS,
        //     "App-> Decoder frame %d process returned - 0x%x)\n",
        //     n, status);

        if (status != VIDDEC_EOK) {
            GT_3trace(curMask, GT_7CLASS,
                "App-> Decoder frame %d processing FAILED, status = 0x%x, "
                "extendedError = 0x%x\n", n, status, decOutArgs.extendedError);
            break;
        }


        /* write to file */
        fwrite(dst[0], OFRAMESIZE, 1, out);
    }

    GT_1trace(curMask, GT_1CLASS, "%d frames encoded/decoded\n", n);
}
/*
 *  @(#) ti.sdo.ce.examples.apps.video_copy; 1, 0, 0,77; 12-2-2010 21:21:28; /db/atree/library/trees/ce/ce-r11x/src/ xlibrary

 */
