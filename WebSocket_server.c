#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <json-c/json.h>
#include <libwebsockets.h>
#include <pthread.h>

#define MOTOR_DEVICE "/dev/motor"
#define MOTOR_TIMEOUT_US 300000

#define MQ135_DEVICE "/dev/mq135"
#define MQ135_READ_BUFFER_SIZE 128
static int mq135_fd = -1;
static int motor_fd = -1;
static volatile sig_atomic_t stop_server;
static unsigned char mq135_tx_buffer[LWS_PRE + 128];
static size_t mq135_tx_length;
static char current_quality[16] = "";
static pthread_t mq135_thread;
static pthread_mutex_t mq135_lock = PTHREAD_MUTEX_INITIALIZER;
static int mq135_update_pending;
struct mq135_client_session{
	unsigned long last_sent_generation;
};
static unsigned long mq135_generation;

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

static void *mq135_worker(void *arg)
{
	struct lws_context *context = arg;

	while(!stop_server){
		int result;
		result = mq135_read_quality();

		if(result ==1){
			pthread_mutex_lock(&mq135_lock);
			mq135_update_pending = 1;
			pthread_mutex_unlock(&mq135_lock);

			lws_cancel_service(context);
		}
			usleep(10000);
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

static int handle_motor_message(const void *data, size_t length)
{
    struct json_object *root;
    struct json_object *type;
    struct json_object *left;
    struct json_object *right;
    int left_value;
    int right_value;
    int ret;

    (void)length;

    root = json_tokener_parse((const char *)data);
    if (!root || !json_object_is_type(root, json_type_object)) {
        motor_stop();
        json_object_put(root);
        return -EINVAL;
    }

    if (!json_object_object_get_ex(root, "type", &type) ||
        strcmp(json_object_get_string(type), "motor") != 0 ||
        !json_object_object_get_ex(root, "left", &left) ||
        !json_object_object_get_ex(root, "right", &right) ||
        !json_object_is_type(left, json_type_int) ||
        !json_object_is_type(right, json_type_int)) {
        motor_stop();
        json_object_put(root);
        return -EINVAL;
    }

    left_value = json_object_get_int(left);
    right_value = json_object_get_int(right);
    ret = motor_set_speed(left_value, right_value);
    if (ret)
        motor_stop();

    json_object_put(root);
    return ret;
}

static int websocket_callback(struct lws *wsi,
                              enum lws_callback_reasons reason,
                              void *user,
                              void *in,
                              size_t len)
{
    struct mq135_client_session *session = user;

    switch (reason) {
    case LWS_CALLBACK_ESTABLISHED:
        session->last_sent_generation = 0;
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
        if (handle_motor_message(in, len) == 0)
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
    case LWS_CALLBACK_SERVER_WRITEABLE:
	int written;
	pthread_mutex_lock(&mq135_lock);
	if(mq135_tx_length == 0 || session->last_sent_generation == mq135_generation){
		pthread_mutex_unlock(&mq135_lock);
		break;
	}
	written = lws_write(
			wsi,
			mq135_tx_buffer + LWS_PRE,
			mq135_tx_length,
			LWS_WRITE_TEXT
			);
	if(written >= 0)
		session->last_sent_generation = mq135_generation;
	
	pthread_mutex_unlock(&mq135_lock);

	if(written < 0)
		return -1;
	break;
    default:
        break;
    }

    return 0;
}

static const struct lws_protocols protocols[] = {
    {
        .name = "smartcar-motor",
        .callback = websocket_callback,
        .per_session_data_size = sizeof(struct mq135_client_session),
        .rx_buffer_size = 4096,
    },
    LWS_PROTOCOL_LIST_TERM
};

int main(void)
{
    struct lws_context_creation_info info;
    struct lws_context *context;

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
    if(pthread_create(&mq135_thread,NULL,mq135_worker,context)!=0){
	    fprintf(stderr,"failed to create MQ135 thread\n");
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
    lws_context_destroy(context);
    close(motor_fd);

    return 0;

}

