#include <stdint.h>
#include <fmt/core.h>
#include "scrcpy_video_decoder.h"
#include <stdlib.h>
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <Windows.h>
#include "string.h"
#include "opencv2/core.hpp"
#include "opencv2/imgcodecs.hpp"
#include <direct.h>
#include <utils.h>
#include <mutex>

extern "C" {
	#include <libavutil/imgutils.h>
	#include <libavutil/samplefmt.h>
	#include <libavutil/timestamp.h>
	#include <libavcodec/avcodec.h>
	#include <libavformat/avformat.h>
	#include <libswscale/swscale.h>
}
using namespace std;
using namespace cv;

#ifndef SCRCPY_DEVICE_INFO_SIZE
#define SCRCPY_DEVICE_INFO_SIZE 68
#define SCRCPY_DEIVCE_ID_LENGTH 64
#define H264_HEAD_BUFFER_SIZE 12
#define PACKET_CHUNK_BUFFER_SIZE 32*1024
#define PNG_IMG_BUFFER 1024 * 1024 * 1
#endif
typedef struct VideoHeader {
	uint64_t pts;
	int length;
} VideoHeader;

typedef struct PacketStat {
	int64_t pts;
	int64_t dts;
	int flags;
}PacketStat;

class VideoDecoder {
private:
	char device_id[SCRCPY_DEIVCE_ID_LENGTH];
	connection_buffer_config *buffer_cfg = NULL;
	image_size *img_size = NULL;
	SOCKET socket = INVALID_SOCKET;
	video_decode_callback *callback = NULL;
	char header_buffer[H264_HEAD_BUFFER_SIZE];
	struct AVCodec *codec = NULL;
	AVCodecContext *codec_ctx = NULL;
	AVCodecParserContext *codec_parser_context = NULL;
	AVPacket *active_packet = NULL;
	AVFrame *frame = NULL;
	struct PacketStat packet_stat;
	char *active_data = NULL;
	char *packet_buffer = NULL;
	char packet_chunk[PACKET_CHUNK_BUFFER_SIZE];
	int pending_data_length = 0;
	BOOL has_pending = FALSE;
	int width = 0;
	int height = 0;
	int *keep_running = NULL;
	std::vector<uchar> *img_buffer = NULL;
	std::mutex img_buffer_lock;
	/*
	* ��ȡ�豸��Ϣ
	*/
	int read_device_info();
	/*
	* ��ʼ��������
	*/
	int init_decoder();
	/*
	* ��ȡ��Ƶͷ��Ϣ
	* @param header ͷ��Ϣ
	* @return �ɹ���ȡ��ͷ��С
	*/
	int read_video_header(struct VideoHeader *header);
	/*
	* ��Ƶ����
	* @param pts
	* @param length
	* @return ״̬��
	*/
	int decode_frames(uint64_t pts, int length);
	/*
	* �������������
	* @param length
	* @return ״̬��
	*/
	int recv_network_buffer(int length, char* buffer, char* chunk);
	/*
	* ׼����packet���ڽ���
	* @param pts
	* @param length
	* @return ״̬��
	*/
	int prepare_packet(uint64_t pts, int length);

	int rgb_frame_and_callback(AVCodecContext* dec_ctx, AVFrame* frame);

	image_size* get_image_size();

public:
	VideoDecoder(SOCKET socket, video_decode_callback *callback, connection_buffer_config* buffer_cfg,
		int *keep_running, std::vector<uchar>* img_buffer);
	~VideoDecoder(void);
	int decode();
	void free_resources();
};
void show_data(char* data, int length) {
	for (int i = 0; i < length; i++) {
		if (i % 32 == 0 && i > 0) {
			fmt::print("\n");
		}
		fmt::print("{0:#x} ", (uint8_t)data[i]);
	}
	fmt::print("\n");
}
VideoDecoder::VideoDecoder(SOCKET socket, video_decode_callback *callback, connection_buffer_config* buffer_cfg,
	int* keep_running, std::vector<uchar>* img_buffer) {
	this->socket = socket;
	this->callback = callback;
	this->buffer_cfg = buffer_cfg;
	this->keep_running = keep_running;
	this->img_buffer = img_buffer;
}
void VideoDecoder::free_resources() {
}
int VideoDecoder::read_device_info() {
	int buf_size = SCRCPY_DEVICE_INFO_SIZE;
	char device_info_data[SCRCPY_DEVICE_INFO_SIZE];
	if (!device_info_data) {
		fmt::print("No enough ram for reading device info from socket {} \n", this->socket);
		return 1;
	}
	memset(device_info_data, 0, buf_size);
	fmt::print("Trying to read device info from socket {} \n", this->socket);
	int bytes_read = recv(this->socket, device_info_data, buf_size, 0);
	fmt::print("{} bytes received from socket {} \n", bytes_read, this->socket);
	if (bytes_read < SCRCPY_DEVICE_INFO_SIZE) {
		return 1;
	}
	// device id is 64 bytes in total
	int device_size_bytes = 2;
	device_info_data[SCRCPY_DEIVCE_ID_LENGTH - 1] = '\0';
	array_copy_to(device_info_data, this->device_id, 0, SCRCPY_DEIVCE_ID_LENGTH);
	this->width = to_int(device_info_data, SCRCPY_DEVICE_INFO_SIZE, SCRCPY_DEIVCE_ID_LENGTH, device_size_bytes);
	this->height = to_int(device_info_data, SCRCPY_DEVICE_INFO_SIZE, SCRCPY_DEIVCE_ID_LENGTH + device_size_bytes, device_size_bytes);
	fmt::print("Device {} connected, width: {}, height: {}, socket: {}\n", this->device_id, this->width, this->height, this->socket);
	// callback for device info
	if (this->callback) {
		this->callback->on_device_info(device_id, this->width, this->height);
	}
	return 0;
}
VideoDecoder::~VideoDecoder() {
	fmt::print("Cleaning video decoder for {}\n", this->socket);
	if (NULL != this->frame) {
		av_frame_free(&this->frame);
	}
	if (NULL != this->codec_ctx) {
		avcodec_free_context(&this->codec_ctx);
	}
	if (NULL != this->codec_parser_context) {
		av_parser_close(this->codec_parser_context);
	}
	if (NULL != this->active_packet) {
		av_packet_free(&this->active_packet);
	}
	if (NULL != this->packet_buffer) {
		free(this->packet_buffer);
	}
	if (NULL != this->active_data) {
		free(this->active_data);
	}
	std::lock_guard<std::mutex> lock_guard{ this->img_buffer_lock };
}
int VideoDecoder::init_decoder() {
	int result = 0;
	connection_buffer_config* cfg = this->buffer_cfg;
	//�����ڴ�
	this->packet_buffer = (char *)malloc(cfg->video_packet_buffer_size_kb * 1024);
	if (!this->packet_buffer) {
		fmt::print("No enough memory for packet buffer\n");
		return -1;
	}
	this->active_data = (char*)malloc(cfg->video_packet_buffer_size_kb * 1024);
	if (!this->active_data) {
		fmt::print("No enough memory for active_data\n");
		return -1;
	}
	enum AVCodecID h264 = AV_CODEC_ID_H264;
	const AVCodec *codec = (AVCodec *)avcodec_find_decoder(h264);
	if (!codec) {
		fmt::print("Could not find h264 codec\n");
		return -1;
	}
	AVCodecContext* codec_context = avcodec_alloc_context3(codec);
	if (!codec_context) {
		fmt::print("No enough memory for codec_context\n");
		return -1;
	}
	if (avcodec_open2(codec_context, codec, NULL) != 0) {
		fmt::print("Failed to open codec\n");
		avcodec_free_context(&codec_context);
		return -1;
	}
	AVCodecParserContext* codec_parser_context = av_parser_init(h264);
	if (!codec_parser_context) {
		fmt::print("Failed to init paser context\n");
		avcodec_free_context(&codec_context);
		return -1;
	}
	codec_parser_context->flags = codec_parser_context->flags | PARSER_FLAG_COMPLETE_FRAMES;
	AVPacket* packet = av_packet_alloc();
	if (!packet) {
		fmt::print("Failed to init packet\n");
		avcodec_free_context(&codec_context);
		av_parser_close(codec_parser_context);
		return -1;
	}
	this->codec = const_cast<AVCodec*>(codec);
	this->codec_ctx = codec_context;
	this->codec_parser_context = codec_parser_context;
	this->active_packet = packet;
	return result;
}
int VideoDecoder::read_video_header(struct VideoHeader* header) {
	char* header_buffer = this->header_buffer;
	fmt::print("Trying to read video header({} bytes) from {} into {} \n", H264_HEAD_BUFFER_SIZE, this->socket, (uintptr_t)header_buffer);
	int bytes_received = recv(this->socket, header_buffer, H264_HEAD_BUFFER_SIZE, 0);
	if (bytes_received != H264_HEAD_BUFFER_SIZE) {
		fmt::print("Error, Read {}/{} for video header\n", bytes_received, H264_HEAD_BUFFER_SIZE);
		return 1;
	}
	uint64_t pts = to_long(header_buffer, bytes_received, 0, 8);
	int length = to_int(header_buffer, bytes_received, 8, 4);
	header->length = length;
	header->pts = pts;
	return bytes_received;
}
int VideoDecoder::recv_network_buffer(int length, char* buffer, char* chunk) {
	int result = 0;
	int read_total = 0;
	int max_chunk = PACKET_CHUNK_BUFFER_SIZE;
	while (read_total < length) {
		int chunk_read_plan = min(max_chunk, length - read_total);
		int read_length = recv(this->socket, chunk, chunk_read_plan, 0);
		if (read_length != chunk_read_plan) {
			fmt::print("Planned to read {} bytes, got {} bytes instead socket={}\n", chunk_read_plan, read_length, this->socket);
		}
		else if (read_length <= 0) {
			result = -1;
			break;
		}
		int fill_start_index = read_total;
		read_total += read_length;
		fmt::print("Receiving {}/{} from network for socket {}\n", read_total, length, this->socket);
		array_copy_to(chunk, buffer, fill_start_index, read_length);
	}
	return result;
}
int VideoDecoder::prepare_packet(uint64_t pts, int length) {
	int result = 0;
	AVPacket* active_packet = this->active_packet;
	BOOL has_pending = this->has_pending;

	active_packet->size = length;
	active_packet->pts = (pts == -1 ? AV_NOPTS_VALUE : pts);

	BOOL is_config = active_packet->pts == AV_NOPTS_VALUE;

	this->packet_stat.pts = active_packet->pts;
	this->packet_stat.dts = active_packet->dts;
	this->packet_stat.flags = active_packet->flags;
	
	fmt::print("is_config = {}, has_pending = {} for socket {}\n", is_config ? "yes" : "no", has_pending ? "yes" : "no", this->socket);
	if (is_config || has_pending) {
		int offset = 0;
		if (has_pending) {
			fmt::print("Detected pending, offset will be {} \n", this->pending_data_length);
			offset = this->pending_data_length;
		}
		else {
			fmt::print("no pending data, saving received data to pending buffer for socket {}\n", this->socket);
			array_copy_to(this->packet_buffer, this->active_data, 0, length);
			this->pending_data_length = length;
			this->has_pending = TRUE;
		}
		if (offset > 0) {
			int new_size = this->pending_data_length + length;
			fmt::print("Existed pending data size = {}, current pending size = {}, final size={}, socket={}\n", this->pending_data_length, length, 
				new_size, this->socket);
			array_copy_to(this->packet_buffer, this->active_data, this->pending_data_length, length);
			this->pending_data_length = 0;
			active_packet->data = (uint8_t *)this->active_data;
			active_packet->size = new_size;
			this->has_pending = FALSE;
		}
		if (!is_config) {
			fmt::print("Preparing active packet\n");
			active_packet->data = NULL;
			int old_size = active_packet->size;

			av_packet_unref(active_packet);

			active_packet->data = (uint8_t*)this->active_data;
			active_packet->dts = this->packet_stat.dts;
			active_packet->flags = this->packet_stat.flags;
			active_packet->pts = this->packet_stat.pts;
			active_packet->size = old_size;
		}
	}
	else {
		this->pending_data_length = 0;
		active_packet->data = (uint8_t*)this->packet_buffer;
	}
	if (is_config) {
		fmt::print("In configuring, will not call decoder for socket {}\n", this->socket);
		active_packet->data = NULL;
		av_packet_unref(active_packet);
		result = 1;
	}
	return result;
}
image_size* VideoDecoder::get_image_size() {
	if (NULL == this->callback) {
		fmt::print("no image size provider configured");
		return NULL;
	}
	if (NULL == this->device_id) {
		fmt::print("no device id provided");
		return NULL;
	}
	return this->callback->get_configured_img_size(this->device_id);
}
int frame_count = 1;
int VideoDecoder::rgb_frame_and_callback(AVCodecContext* dec_ctx, AVFrame* frame) {
	struct SwsContext *sws_ctx = NULL;
	int width = frame->width;
	int height = frame->height;

	int cv_line_size[1];
	int target_width = width;
	int target_height = height;

	image_size* configrued_size = this->get_image_size();
	if (NULL != configrued_size && configrued_size->width > 0 && configrued_size->height > 0) {
		target_width = configrued_size->width;
		target_height = configrued_size->height;
		fmt::print("Resizing image from {}x{} to {}x{}\n", width, height, target_width, target_height);
	}

	cv::Mat image(target_height, target_width, CV_8UC4);
	cv_line_size[0] = (int)image.step1();

	sws_ctx = sws_getContext(dec_ctx->width,
		dec_ctx->height,
		dec_ctx->pix_fmt,
		target_width,
		target_height,
		AV_PIX_FMT_RGB32,
		SWS_BICUBIC,
		NULL,
		NULL,
		NULL);
	if (NULL == sws_ctx) {
		return 1;
	}
	sws_scale(sws_ctx, frame->data, frame->linesize, 0, height, &image.data, cv_line_size);
	sws_freeContext(sws_ctx);
	std::lock_guard<std::mutex> lock_guard{ this->img_buffer_lock };
	fmt::print("Encoding image to png format");
	if (cv::imencode(".png", image, *this->img_buffer)) {
		image.release();
		int img_size = this->img_buffer->size();
		fmt::print("sending {} bytes to callback\n", img_size);
		uint8_t* img_data = (uint8_t*)this->img_buffer->data();
		this->callback->on_video_callback(device_id, img_data, this->img_buffer->size(), target_width, target_height, width, height);
	} else {
		fmt::print(stderr, "Failed to encode a png file\n");
	}
	return 0;
}
int VideoDecoder::decode_frames(uint64_t pts, int length) {
	int result = 0;
	BOOL reset_has_pending = FALSE;
	int status = 0;
	AVFrame* frame = NULL;
	fmt::print("decode_frames pts={} length={} socket={}\n", pts, length, this->socket);
	result = this->recv_network_buffer(length, this->packet_buffer, this->packet_chunk);
	// failed to receiving data
	if (result != 0) {
		return -1;
	}
	result = this->prepare_packet(pts, length);
	// no need to do decoding
	if (result == 1) {
		return result;
	}
	fmt::print("Fetching codec parser context for socekt {}\n", this->socket);
	AVCodecParserContext* parser_context = this->codec_parser_context;
	if (parser_context->key_frame == 1) {
		active_packet->flags = (active_packet->flags | AV_PKT_FLAG_KEY);
		fmt::print("Confiuring flags for socket {}\n", this->socket);
	}
	fmt::print("Fetching codec context for socket {}\n", this->socket);
	AVCodecContext* codec_context = this->codec_ctx;
	fmt::print("Sending packet for decoding, data pointer address is {} size={} socket={}\n", (uintptr_t)active_packet->data,
		active_packet->size, this->socket);
	result = avcodec_send_packet(codec_context, active_packet);
	if (result != 0) {
		reset_has_pending = TRUE;
		fmt::print("Could not invoke avcodec_send_packet: {} socket={}\n", result, this->socket);
		active_packet->data = NULL;
		av_packet_unref(active_packet);
		goto end;
	}
	if (NULL == this->frame) {
		this->frame = av_frame_alloc();
		if (!this->frame) {
			result = -1;
			goto end;
		}
	}
	frame = this->frame;
	while (status >= 0) {
		status = avcodec_receive_frame(codec_context, frame);
		if (status == 0) {
			fmt::print("Got frame with width={} height={} socket={}\n", frame->width, frame->height, this->socket);
			this->rgb_frame_and_callback(codec_context, frame);
		}
		else if (status == AVERROR(EAGAIN)) {
			active_packet->data = NULL;
			av_packet_unref(active_packet);
			reset_has_pending = TRUE;
			goto end;
		}
		else if (status == AVERROR_EOF) {
			reset_has_pending = TRUE;
			break;
		}
	}
	end:
		if (has_pending && reset_has_pending) {
			fmt::print("Reset has_pending=false for socket {}\n", this->socket);
			this->has_pending = FALSE;
		}
		return result;
}
int VideoDecoder::decode() {
	if (this->read_device_info()) {
		fmt::print("Failed to read device info for socket {} \n", this->socket);
		return 1;
	}
	if (this->init_decoder() != 0) {
		fmt::print("Failed to init decoder for socket {} \n", this->socket);
		return 1;
	}
	struct VideoHeader header;
	int keep_connection = 1;
	int status = 0;
	while (*this->keep_running == 1 && keep_connection == 1) {
		int header_size = this->read_video_header(&header);
		if (header_size <= 0) {
			keep_connection = 0;
			status = 1;
			fmt::print("Failed to read header info from {}\n", this->socket);
			break;
		}
		if (header_size != H264_HEAD_BUFFER_SIZE) {
			status = 1;
			fmt::print("Failed to read header info from {}\n", this->socket);
			break;
		}
		int decode_status = this->decode_frames(header.pts, header.length);
		if (decode_status == -1) {
			fmt::print("Bad status for decoding video from {}, will not continue\n", this->socket);
			break;
		}
	}
	return status;
}
int socket_decode(SOCKET socket, video_decode_callback *callback, connection_buffer_config* buffer_cfg,
	int *keep_running) {
	std::vector<uchar> * image_buffer = new std::vector<uchar>(PNG_IMG_BUFFER);
	int result_code = 0;
	VideoDecoder *decoder = new VideoDecoder(socket, callback, 
		buffer_cfg, keep_running, 
		image_buffer);
	int result = decoder->decode();

	delete image_buffer;
	delete decoder;
	return result;
}