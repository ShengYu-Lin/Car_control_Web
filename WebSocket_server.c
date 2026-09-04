#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <time.h>
#include <json-c/json.h>
#include <libwebsockets.h>
#include <pthread.h>

#define MOTOR_DEVICE "/dev/motor"
#define MOTOR_TIMEOUT_US 300000

#define MQ135_DEVICE "/dev/mq135"
#define MQ135_READ_BUFFER_SIZE 128

/* ── 雲台：Pico W，走 UART ─────────────────────────────────────
 *
 * 前端按住方向鍵時每 120 ms 送一次 camera 指令，每次轉
 * GIMBAL_STEP_DEG 度。協定是一行 JSON 加換行，Pico 端用 json.loads()
 * 收，格式跟 Pi 上的 server.py 完全一樣。
 *
 * Pico 沒插上不會讓伺服器起不來 —— 馬達和空氣品質要照常能用，
 * 雲台自己每 3 秒重試一次。
 */
#define PICO_DEVICE      "/dev/ttyAMA0"
#define PICO_BAUD        B115200
#define PICO_RETRY_MS    3000
#define GIMBAL_STEP_DEG  3.0
#define PAN_MIN          (-90.0)
#define PAN_MAX          90.0
#define TILT_MIN         (-45.0)
#define TILT_MAX         45.0
/* 內部記帳一律「正值 = 右 / 上」，只有真正送出去時才依安裝方向反號。
 * 裝好之後如果轉的方向相反，改這兩個就好，其他地方都不用動。 */
#define PAN_REVERSED     1
#define TILT_REVERSED    1
#define GIMBAL_HOLD_MS   300   /* 動作期間定期重送，維持伺服扭力 */
#define GIMBAL_IDLE_MS   500   /* 停這麼久就不再補送，Pico 韌體會自己放開 */

/* 定期推播給前端的間隔 */
#define STATUS_PERIOD_MS 250
#define IMU_PERIOD_MS    200
#define WORKER_TICK_US   10000

static int mq135_fd = -1;
static int motor_fd = -1;
static volatile sig_atomic_t stop_server;
static unsigned char mq135_tx_buffer[LWS_PRE + 128];
static size_t mq135_tx_length;
static char current_quality[16] = "";
static pthread_t mq135_thread;
static pthread_mutex_t mq135_lock = PTHREAD_MUTEX_INITIALIZER;
static int mq135_update_pending;

/* 每個連線各自記錄「已經送到第幾代」，三種推播互不干擾 */
struct client_session{
	unsigned long last_sent_generation;     /* 空氣品質 */
	unsigned long last_status_generation;   /* 模式與雲台角度 */
	unsigned long last_imu_generation;      /* 車身姿態 */
};
static unsigned long mq135_generation;

/* state_lock 保護底下這一整組：雲台狀態、模式、以及 status/imu 的世代。
 * 跟 mq135_lock 是兩把獨立的鎖，程式裡不會同時持有兩把。 */
static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;
static int pico_fd = -1;
static double gimbal_pan;
static double gimbal_tilt;
static int64_t gimbal_last_move_ms;
static int64_t gimbal_last_sent_ms;
static int64_t gimbal_retry_ms;
static int gimbal_active;
static char current_mode[8] = "manual";   /* 前端開頁面也是預設手動 */
static unsigned long status_generation;
static unsigned long imu_generation;
static int periodic_update_pending;

static int64_t now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int mq135_read_quality(void)
{
	char buffer[MQ135_READ_BUFFER_SIZE];
	char quality[16];
	ssize_t length;

	length = read(mq135_fd,buffer,sizeof(buffer) -1);
	if(length < 0){
		if(errno == EAGAIN || errno == EWOULDBLOCK)
			return 0;
		return -errno;
	}
	if(length == 0){
		return 0;
	}
	buffer[length] ='\0';
	fprintf(stderr,"MQ135 received: %s",buffer);
	if(sscanf(buffer,"RAW=%*u MV=%*u QUALITY=%15[^\n]",quality)!=1){
		return -EINVAL;
	}
	if(strcmp(quality,"GOOD")!= 0&&
	   strcmp(quality,"NORMAL")!=0&&
	   strcmp(quality,"BAD")!= 0&&
	   strcmp(quality,"UNKNOWN")!= 0){
		return -EINVAL;
	}
	pthread_mutex_lock(&mq135_lock);

	if(strcmp(current_quality,quality) == 0)
	{
		pthread_mutex_unlock(&mq135_lock);
		return 0;
	}

	snprintf(current_quality,sizeof(current_quality),"%s",quality);
	
	mq135_tx_length=(size_t)snprintf(
			(char*)mq135_tx_buffer + LWS_PRE,
			sizeof(mq135_tx_buffer) - LWS_PRE,
			"{\"type\":\"mq135\",\"quality\":\"%s\"}",
			current_quality
			);
	mq135_generation++;
	pthread_mutex_unlock(&mq135_lock);
	return 1;
}

/* ── 雲台 ────────────────────────────────────────────────────
 *
 * 底下標了 _locked 的函式都要在持有 state_lock 的情況下呼叫。
 */

static double clamp_deg(double value, double low, double high)
{
    if (value < low)
        return low;
    if (value > high)
        return high;
    return value;
}

static void gimbal_close_locked(void)
{
    if (pico_fd >= 0) {
        close(pico_fd);
        pico_fd = -1;
    }
}

static int gimbal_open_locked(void)
{
    struct termios tio;
    int fd;
    int error;

    fd = open(PICO_DEVICE, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return -errno;

    if (tcgetattr(fd, &tio) < 0) {
        error = -errno;
        close(fd);
        return error;
    }

    cfmakeraw(&tio);
    cfsetispeed(&tio, PICO_BAUD);
    cfsetospeed(&tio, PICO_BAUD);
    tio.c_cflag |= CLOCAL | CREAD;
    tio.c_cflag &= ~CRTSCTS;
    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = 0;

    if (tcsetattr(fd, TCSANOW, &tio) < 0) {
        error = -errno;
        close(fd);
        return error;
    }

    pico_fd = fd;
    return 0;
}

static void gimbal_write_locked(void)
{
    char command[96];
    double pan;
    double tilt;
    int length;

    if (pico_fd < 0)
        return;

    pan = PAN_REVERSED ? -gimbal_pan : gimbal_pan;
    tilt = TILT_REVERSED ? -gimbal_tilt : gimbal_tilt;

    length = snprintf(command, sizeof(command),
                      "{\"pan\": %.1f, \"tilt\": %.1f}\n", pan, tilt);
    if (write(pico_fd, command, (size_t)length) != length) {
        lwsl_notice("gimbal: write failed (%s); reconnecting\n", strerror(errno));
        gimbal_close_locked();
        gimbal_retry_ms = now_ms();
        return;
    }

    gimbal_last_sent_ms = now_ms();
}

static void gimbal_step(const char *direction)
{
    pthread_mutex_lock(&state_lock);

    if (strcmp(direction, "left") == 0)
        gimbal_pan = clamp_deg(gimbal_pan - GIMBAL_STEP_DEG, PAN_MIN, PAN_MAX);
    else if (strcmp(direction, "right") == 0)
        gimbal_pan = clamp_deg(gimbal_pan + GIMBAL_STEP_DEG, PAN_MIN, PAN_MAX);
    else if (strcmp(direction, "up") == 0)
        gimbal_tilt = clamp_deg(gimbal_tilt + GIMBAL_STEP_DEG, TILT_MIN, TILT_MAX);
    else if (strcmp(direction, "down") == 0)
        gimbal_tilt = clamp_deg(gimbal_tilt - GIMBAL_STEP_DEG, TILT_MIN, TILT_MAX);
    else {
        pthread_mutex_unlock(&state_lock);
        lwsl_notice("gimbal: unknown direction %s\n", direction);
        return;
    }

    gimbal_last_move_ms = now_ms();
    gimbal_active = 1;
    gimbal_write_locked();

    pthread_mutex_unlock(&state_lock);
}

/* 由背景執行緒每 10 ms 呼叫：維持扭力、記錄停止位置、斷線時重連。
 * 伺服待機的抖動與嗡嗡聲由 Pico 韌體自己處理（停 0.5 秒後切斷 PWM），
 * 這邊不需要再送放開指令。 */
static void gimbal_tick(void)
{
    char scratch[128];
    int64_t now;

    now = now_ms();

    pthread_mutex_lock(&state_lock);

    if (gimbal_active && now - gimbal_last_move_ms > GIMBAL_IDLE_MS) {
        gimbal_active = 0;
        lwsl_notice("gimbal: idle at pan=%.0f tilt=%.0f\n", gimbal_pan, gimbal_tilt);
    }

    if (pico_fd < 0) {
        if (now - gimbal_retry_ms >= PICO_RETRY_MS) {
            gimbal_retry_ms = now;
            if (gimbal_open_locked() == 0) {
                lwsl_notice("gimbal: connected to %s\n", PICO_DEVICE);
                gimbal_write_locked();
            }
        }
    } else {
        if (gimbal_active && now - gimbal_last_sent_ms >= GIMBAL_HOLD_MS)
            gimbal_write_locked();

        /* Pico 偶爾會回訊，不讀會塞滿核心緩衝，讀了直接丟掉 */
        while (read(pico_fd, scratch, sizeof(scratch)) > 0)
            ;
    }

    pthread_mutex_unlock(&state_lock);
}

/* 背景執行緒：輪詢空氣品質、驅動雲台的保持扭力、按時間戳推播狀態與姿態。
 * 全部擠在同一條執行緒是刻意的 —— 這些工作都很輕，10 ms 一輪綽綽有餘，
 * 多開執行緒只會多一組同步問題。 */
static void *sensor_worker(void *arg)
{
	struct lws_context *context = arg;
	int64_t next_status = now_ms();
	int64_t next_imu = now_ms();

	while(!stop_server){
		int result;
		int wake = 0;
		int64_t now;

		result = mq135_read_quality();

		if(result ==1){
			pthread_mutex_lock(&mq135_lock);
			mq135_update_pending = 1;
			pthread_mutex_unlock(&mq135_lock);
			wake = 1;
		}

		gimbal_tick();

		now = now_ms();
		pthread_mutex_lock(&state_lock);
		if(now >= next_status){
			next_status = now + STATUS_PERIOD_MS;
			status_generation++;
			periodic_update_pending = 1;
			wake = 1;
		}
		if(now >= next_imu){
			next_imu = now + IMU_PERIOD_MS;
			imu_generation++;
			periodic_update_pending = 1;
			wake = 1;
		}
		pthread_mutex_unlock(&state_lock);

		if(wake)
			lws_cancel_service(context);

		usleep(WORKER_TICK_US);
	}
		return NULL;
}

static void handle_signal(int signal_number)
{
    (void)signal_number;
    stop_server = 1;
}

static void motor_stop(void)
{
    static const char command[] = "0 0\n";

    if (motor_fd >= 0)
        (void)write(motor_fd, command, sizeof(command) - 1);
}

static int motor_set_speed(int left, int right)
{
    char command[32];
    int length;

    if (left < -100 || left > 100 || right < -100 || right > 100)
        return -ERANGE;

    length = snprintf(command, sizeof(command), "%d %d\n", left, right);
    if (write(motor_fd, command, (size_t)length) != length)
        return -errno;

    return 0;
}

/* 回傳 1 表示這是一筆有效的 motor 指令，呼叫端要重設看門狗計時器。 */
static int handle_motor_message(struct json_object *root)
{
    struct json_object *left;
    struct json_object *right;
    int ret;

    if (!json_object_object_get_ex(root, "left", &left) ||
        !json_object_object_get_ex(root, "right", &right) ||
        !json_object_is_type(left, json_type_int) ||
        !json_object_is_type(right, json_type_int)) {
        motor_stop();
        return -EINVAL;
    }

    ret = motor_set_speed(json_object_get_int(left), json_object_get_int(right));
    if (ret) {
        motor_stop();
        return ret;
    }

    return 1;
}

static void handle_camera_message(struct json_object *root)
{
    struct json_object *direction;
    int is_auto;

    pthread_mutex_lock(&state_lock);
    is_auto = strcmp(current_mode, "auto") == 0;
    pthread_mutex_unlock(&state_lock);

    /* 自動模式下方向鍵不生效（前端也會把方向鍵鎖起來） */
    if (is_auto)
        return;

    if (!json_object_object_get_ex(root, "direction", &direction) ||
        !json_object_is_type(direction, json_type_string))
        return;

    gimbal_step(json_object_get_string(direction));
}

static void handle_mode_message(struct json_object *root)
{
    struct json_object *mode;
    const char *name;

    if (!json_object_object_get_ex(root, "mode", &mode) ||
        !json_object_is_type(mode, json_type_string))
        return;

    name = json_object_get_string(mode);
    if (strcmp(name, "manual") != 0 && strcmp(name, "auto") != 0) {
        lwsl_notice("unknown mode: %s\n", name);
        return;
    }

    pthread_mutex_lock(&state_lock);
    snprintf(current_mode, sizeof(current_mode), "%s", name);
    pthread_mutex_unlock(&state_lock);

    lwsl_notice("mode: %s\n", name);
}

static void handle_detect_message(struct json_object *root)
{
    struct json_object *enabled;

    if (!json_object_object_get_ex(root, "enabled", &enabled) ||
        !json_object_is_type(enabled, json_type_boolean))
        return;

    /* 這一版沒有接影像辨識，做不到人臉偵測。收下這個型別只是為了不要
     * 掉進「未知型別」那一支，狀態推播一律回報 detecting:false。 */
    lwsl_notice("face detection requested (%s) but not available here\n",
                json_object_get_boolean(enabled) ? "on" : "off");
}

/* 收到前端訊息的統一入口。
 *
 * 回傳 1 = 有效的 motor 指令，0 = 其他有效訊息，負值 = 解析失敗。
 *
 * 這裡最重要的一條規則：**不是 motor 的訊息絕對不能停車**。前端按住
 * 雲台方向鍵時每 120 ms 就會送一次 camera，重連時還會送 mode 和
 * detect —— 只要其中一種會觸發 motor_stop()，車子就等於開不動。
 */
static int handle_client_message(const void *data, size_t length)
{
    struct json_tokener *tokener;
    struct json_object *root;
    struct json_object *type;
    const char *type_name;
    int result = 0;

    if (!data || length == 0) {
        motor_stop();
        return -EINVAL;
    }

    /* 帶著長度解析。libwebsockets 給的 in 不保證是 NUL 結尾的字串，
     * 直接餵給 json_tokener_parse() 會讀過頭。 */
    tokener = json_tokener_new();
    if (!tokener) {
        motor_stop();
        return -ENOMEM;
    }
    root = json_tokener_parse_ex(tokener, (const char *)data, (int)length);
    json_tokener_free(tokener);

    if (!root || !json_object_is_type(root, json_type_object)) {
        motor_stop();
        json_object_put(root);
        return -EINVAL;
    }

    if (!json_object_object_get_ex(root, "type", &type) ||
        !json_object_is_type(type, json_type_string)) {
        motor_stop();
        json_object_put(root);
        return -EINVAL;
    }

    type_name = json_object_get_string(type);

    if (strcmp(type_name, "motor") == 0)
        result = handle_motor_message(root);
    else if (strcmp(type_name, "camera") == 0)
        handle_camera_message(root);
    else if (strcmp(type_name, "mode") == 0)
        handle_mode_message(root);
    else if (strcmp(type_name, "detect") == 0)
        handle_detect_message(root);
    else
        lwsl_notice("ignoring unknown message type: %s\n", type_name);

    json_object_put(root);
    return result;
}

static int websocket_callback(struct lws *wsi,
                              enum lws_callback_reasons reason,
                              void *user,
                              void *in,
                              size_t len)
{
    struct client_session *session = user;

    switch (reason) {
    case LWS_CALLBACK_ESTABLISHED:
        session->last_sent_generation = 0;
        session->last_status_generation = 0;
        session->last_imu_generation = 0;
	motor_stop();
        lws_set_timer_usecs(wsi, MOTOR_TIMEOUT_US);
	pthread_mutex_lock(&mq135_lock);
	if(current_quality[0] != '\0'){
		mq135_tx_length = (size_t)snprintf(
				(char *)mq135_tx_buffer + LWS_PRE,
				sizeof(mq135_tx_buffer) - LWS_PRE,
				"{\"type\":\"mq135\",\"quality\":\"%s\"}",
				current_quality
				);
	}
	pthread_mutex_unlock(&mq135_lock);

	lws_callback_on_writable(wsi);
        lwsl_notice("WebSocket client connected\n");
        break;

    case LWS_CALLBACK_RECEIVE:
        /* 只有 motor 指令會重設看門狗。camera / mode / detect 不算
         * 「還在開車」，不然放開搖桿改按雲台就永遠不會逾時停車了。 */
        if (handle_client_message(in, len) == 1)
            lws_set_timer_usecs(wsi, MOTOR_TIMEOUT_US);
        break;

    case LWS_CALLBACK_TIMER:
        motor_stop();
        lwsl_notice("Motor command timeout; stopped\n");
        break;

    case LWS_CALLBACK_CLOSED:
        motor_stop();
        lwsl_notice("WebSocket client disconnected; stopped\n");
        break;

    case LWS_CALLBACK_SERVER_WRITEABLE: {
        /* libwebsockets 規定一次 writable 只能送一則訊息，還有別的要送
         * 就再排一次。優先序：空氣品質 > 狀態 > 姿態。 */
        unsigned char payload[LWS_PRE + 256];
        int length = 0;
        int written;
        int more;

        pthread_mutex_lock(&mq135_lock);
        if (mq135_tx_length != 0 &&
            session->last_sent_generation != mq135_generation) {
            memcpy(payload + LWS_PRE, mq135_tx_buffer + LWS_PRE, mq135_tx_length);
            length = (int)mq135_tx_length;
            session->last_sent_generation = mq135_generation;
        }
        pthread_mutex_unlock(&mq135_lock);

        if (length == 0) {
            pthread_mutex_lock(&state_lock);
            if (session->last_status_generation != status_generation) {
                session->last_status_generation = status_generation;
                /* face / locked / patrol / detecting 這一版都沒有對應的
                 * 功能，一律送 false —— 這是誠實的預設值，讓介面顯示
                 * 「未偵測到目標」，而不是停在舊狀態騙人。 */
                length = snprintf((char *)payload + LWS_PRE,
                                  sizeof(payload) - LWS_PRE,
                                  "{\"type\":\"status\",\"mode\":\"%s\","
                                  "\"detecting\":false,\"face\":false,"
                                  "\"locked\":false,\"patrol\":false,"
                                  "\"pan\":%d,\"tilt\":%d}",
                                  current_mode,
                                  (int)(gimbal_pan < 0 ? gimbal_pan - 0.5
                                                       : gimbal_pan + 0.5),
                                  (int)(gimbal_tilt < 0 ? gimbal_tilt - 0.5
                                                        : gimbal_tilt + 0.5));
            }
            pthread_mutex_unlock(&state_lock);
        }

        if (length == 0) {
            pthread_mutex_lock(&state_lock);
            if (session->last_imu_generation != imu_generation) {
                session->last_imu_generation = imu_generation;
                /* 這一版沒有接 MPU6050。照樣推 ok:false，前端才會把
                 * 儀表變灰顯示 "--"，而不是停在最後一個值讓人以為
                 * 車子還在那個角度。 */
                length = snprintf((char *)payload + LWS_PRE,
                                  sizeof(payload) - LWS_PRE,
                                  "{\"type\":\"imu\",\"ok\":false}");
            }
            pthread_mutex_unlock(&state_lock);
        }

        if (length == 0)
            break;

        written = lws_write(wsi, payload + LWS_PRE, (size_t)length,
                            LWS_WRITE_TEXT);
        if (written < 0)
            return -1;

        pthread_mutex_lock(&state_lock);
        more = session->last_status_generation != status_generation ||
               session->last_imu_generation != imu_generation;
        pthread_mutex_unlock(&state_lock);

        if (!more) {
            pthread_mutex_lock(&mq135_lock);
            more = mq135_tx_length != 0 &&
                   session->last_sent_generation != mq135_generation;
            pthread_mutex_unlock(&mq135_lock);
        }

        if (more)
            lws_callback_on_writable(wsi);

        break;
    }

    default:
        break;
    }

    return 0;
}

static const struct lws_protocols protocols[] = {
    {
        .name = "smartcar-motor",
        .callback = websocket_callback,
        .per_session_data_size = sizeof(struct client_session),
        .rx_buffer_size = 4096,
    },
    LWS_PROTOCOL_LIST_TERM
};

int main(void)
{
    struct lws_context_creation_info info;
    struct lws_context *context;
    int gimbal_error;

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    motor_fd = open(MOTOR_DEVICE, O_WRONLY | O_CLOEXEC);
    if (motor_fd < 0) {
        fprintf(stderr, "open %s: %s\n", MOTOR_DEVICE, strerror(errno));
        return 1;
    }
    mq135_fd = open(MQ135_DEVICE,O_RDONLY|O_NONBLOCK|O_CLOEXEC);
    if(mq135_fd<0){
	    fprintf(stderr,"open %s failed: %s\n",MQ135_DEVICE,strerror(errno));
	    close(motor_fd);
	    return 1;
    }

    /* 雲台接不上不算致命 —— 馬達和空氣品質要照常能用，
     * 背景執行緒會每 3 秒重試一次。 */
    pthread_mutex_lock(&state_lock);
    gimbal_error = gimbal_open_locked();
    gimbal_retry_ms = now_ms();
    pthread_mutex_unlock(&state_lock);
    if (gimbal_error == 0)
        lwsl_notice("gimbal: connected to %s\n", PICO_DEVICE);
    else
        fprintf(stderr, "open %s failed: %s (pan/tilt disabled, will retry)\n",
                PICO_DEVICE, strerror(-gimbal_error));

    memset(&info, 0, sizeof(info));
    info.port = 8080;
    info.protocols = protocols;
    info.options = LWS_SERVER_OPTION_VALIDATE_UTF8;

    context = lws_create_context(&info);
    if (!context) {
        fprintf(stderr, "failed to create WebSocket context\n");
	close(mq135_fd);
	close(motor_fd);
        return 1;
    }
    if(pthread_create(&mq135_thread,NULL,sensor_worker,context)!=0){
	    fprintf(stderr,"failed to create sensor thread\n");
	    lws_context_destroy(context);
	    close(mq135_fd);
	    close(motor_fd);
	    return 1;
    }
    lwsl_notice("SmartCar WebSocket server listening on port 8080\n");
    while (!stop_server){
            int update_pending;

	    lws_service(context,0);

	    pthread_mutex_lock(&mq135_lock);
	    update_pending = mq135_update_pending;
	    mq135_update_pending = 0;
	    pthread_mutex_unlock(&mq135_lock);

	    pthread_mutex_lock(&state_lock);
	    update_pending |= periodic_update_pending;
	    periodic_update_pending = 0;
	    pthread_mutex_unlock(&state_lock);

	    if(update_pending){
		    lws_callback_on_writable_all_protocol(
				    context,
				    &protocols[0]);
	    }
    }
    stop_server = 1;
    lws_cancel_service(context);
    pthread_join(mq135_thread,NULL);

    close(mq135_fd);
    motor_stop();
    pthread_mutex_lock(&state_lock);
    gimbal_close_locked();
    pthread_mutex_unlock(&state_lock);
    lws_context_destroy(context);
    close(motor_fd);

    return 0;

}
