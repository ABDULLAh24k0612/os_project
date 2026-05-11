#include <gtk/gtk.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <semaphore.h>
#include <signal.h>
#include <unistd.h>
#include <stdlib.h>
#include <time.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>

#define FLEET_CAPACITY 5
#define COMMUTER_VOL 15

typedef struct {
    int transit_id;
    int occupancy_status;
    int total_trips;
    float journey_progress;
} TransitUnit;

typedef struct {
    sem_t fleet_guard;
    sem_t stats_guard;
    sem_t available_transit_slots;

    int active_requests;
    int finalized_trips;

    TransitUnit transit_fleet[FLEET_CAPACITY];
} SharedSystemState;

SharedSystemState *shm;
int log_pipe[2];
pid_t commuter_pids[COMMUTER_VOL];
int sim_running = 0;

GtkWidget *stats_label;
GtkWidget *fleet_progress[FLEET_CAPACITY];
GtkWidget *fleet_labels[FLEET_CAPACITY];
GtkTextBuffer *log_buffer;

void log_event(const char* message) {
    write(log_pipe[1], message, strlen(message));
}

int fetch_idle_transit_load_balanced() {
    int best_idx = -1;
    int min_trips = 999999;

    for (int k = 0; k < FLEET_CAPACITY; k++) {
        if (shm->transit_fleet[k].occupancy_status == 0) {
            if (shm->transit_fleet[k].total_trips < min_trips) {
                min_trips = shm->transit_fleet[k].total_trips;
                best_idx = k;
            }
        }
    }

    if (best_idx != -1) {
        shm->transit_fleet[best_idx].occupancy_status = 1;
        shm->transit_fleet[best_idx].total_trips++;
        shm->transit_fleet[best_idx].journey_progress = 0.0;
    }

    return best_idx;
}

void commuter_process_routine(int commuter_id) {
    srand(time(NULL) ^ (getpid() << 16));
    char msg[256];

    snprintf(msg, sizeof(msg), "[System] Commuter %d entered the queue.\n", commuter_id);
    log_event(msg);

    sem_wait(&shm->stats_guard);
    shm->active_requests++;
    sem_post(&shm->stats_guard);

    sem_wait(&shm->available_transit_slots);

    sem_wait(&shm->fleet_guard);
    int matched_idx = fetch_idle_transit_load_balanced();
    sem_post(&shm->fleet_guard);

    snprintf(msg, sizeof(msg), "[Dispatch] Commuter %d matched with Transit %d.\n",
             commuter_id,
             shm->transit_fleet[matched_idx].transit_id);

    log_event(msg);

    int travel_steps = 20;
    int travel_time = (rand() % 200 + 300) * 1000;

    for(int step = 1; step <= travel_steps; step++) {
        usleep(travel_time);

        sem_wait(&shm->fleet_guard);
        shm->transit_fleet[matched_idx].journey_progress =
            (float)step / travel_steps;
        sem_post(&shm->fleet_guard);
    }

    snprintf(msg,
             sizeof(msg),
             "[Arrival] Transit %d dropped off Commuter %d.\n",
             shm->transit_fleet[matched_idx].transit_id,
             commuter_id);

    log_event(msg);

    sem_wait(&shm->fleet_guard);

    shm->transit_fleet[matched_idx].occupancy_status = 0;
    shm->transit_fleet[matched_idx].journey_progress = 0.0;

    sem_post(&shm->fleet_guard);

    sem_wait(&shm->stats_guard);
    shm->finalized_trips++;
    shm->active_requests--;
    sem_post(&shm->stats_guard);

    sem_post(&shm->available_transit_slots);

    exit(0);
}

void handle_sigchld(int sig) {
    while (waitpid(-1, NULL, WNOHANG) > 0);
}

void cleanup_system() {
    for (int i = 0; i < COMMUTER_VOL; i++) {
        if (commuter_pids[i] > 0)
            kill(commuter_pids[i], SIGKILL);
    }

    sem_destroy(&shm->fleet_guard);
    sem_destroy(&shm->stats_guard);
    sem_destroy(&shm->available_transit_slots);

    munmap(shm, sizeof(SharedSystemState));

    gtk_main_quit();
}

void handle_sigint(int sig) {
    cleanup_system();
    exit(0);
}

void* commuter_spawner_routine(void* arg) {
    for (int i = 0; i < COMMUTER_VOL; i++) {

        pid_t pid = fork();

        if (pid == 0) {
            commuter_process_routine(i + 1);
        }
        else {
            commuter_pids[i] = pid;

            int wait_time_ms = (rand() % 1400) + 100;
            usleep(wait_time_ms * 1000);
        }
    }

    return NULL;
}

void on_start_clicked(GtkWidget *widget, gpointer data) {

    if (sim_running)
        return;

    sim_running = 1;

    log_event("[Control] Simulation Started...\n");

    pthread_t spawner_thread;

    pthread_create(&spawner_thread,
                   NULL,
                   commuter_spawner_routine,
                   NULL);

    pthread_detach(spawner_thread);
}

void on_pause_clicked(GtkWidget *widget, gpointer data) {

    log_event("[Control] Simulation Paused via SIGSTOP.\n");

    for (int i = 0; i < COMMUTER_VOL; i++) {

        if (commuter_pids[i] > 0)
            kill(commuter_pids[i], SIGSTOP);
    }
}

void on_resume_clicked(GtkWidget *widget, gpointer data) {

    log_event("[Control] Simulation Resumed via SIGCONT.\n");

    for (int i = 0; i < COMMUTER_VOL; i++) {

        if (commuter_pids[i] > 0)
            kill(commuter_pids[i], SIGCONT);
    }
}

gboolean update_gui_state(gpointer data) {

    char buffer[256];

    char log_buf[1024];

    int bytes_read =
        read(log_pipe[0],
             log_buf,
             sizeof(log_buf) - 1);

    if (bytes_read > 0) {

        log_buf[bytes_read] = '\0';

        GtkTextIter iter;

        gtk_text_buffer_get_end_iter(log_buffer, &iter);

        gtk_text_buffer_insert(log_buffer,
                               &iter,
                               log_buf,
                               -1);
    }

    sem_wait(&shm->stats_guard);

    snprintf(buffer,
             sizeof(buffer),
             "<span size='large'><b>Active Requests:</b> %d\n<b>Finalized Trips:</b> %d / %d</span>",
             shm->active_requests,
             shm->finalized_trips,
             COMMUTER_VOL);

    gtk_label_set_markup(GTK_LABEL(stats_label), buffer);

    sem_post(&shm->stats_guard);

    sem_wait(&shm->fleet_guard);

    for(int i = 0; i < FLEET_CAPACITY; i++) {

        const char* status =
            (shm->transit_fleet[i].occupancy_status == 1)
            ? "<span foreground='red'>EN ROUTE</span>"
            : "<span foreground='green'>IDLE</span>";

        snprintf(buffer,
                 sizeof(buffer),
                 "<b>Transit Unit %d</b> | Status: %s | Trips: %d",
                 shm->transit_fleet[i].transit_id,
                 status,
                 shm->transit_fleet[i].total_trips);

        gtk_label_set_markup(GTK_LABEL(fleet_labels[i]), buffer);

        gtk_progress_bar_set_fraction(
            GTK_PROGRESS_BAR(fleet_progress[i]),
            shm->transit_fleet[i].journey_progress
        );
    }

    sem_post(&shm->fleet_guard);

    return TRUE;
}

int main(int argc, char *argv[]) {

    signal(SIGINT, handle_sigint);
    signal(SIGCHLD, handle_sigchld);

    shm = mmap(NULL,
               sizeof(SharedSystemState),
               PROT_READ | PROT_WRITE,
               MAP_SHARED | MAP_ANONYMOUS,
               -1,
               0);

    sem_init(&shm->fleet_guard, 1, 1);
    sem_init(&shm->stats_guard, 1, 1);
    sem_init(&shm->available_transit_slots,
             1,
             FLEET_CAPACITY);

    shm->active_requests = 0;
    shm->finalized_trips = 0;

    for (int k = 0; k < FLEET_CAPACITY; k++) {

        shm->transit_fleet[k].transit_id = k + 100;
        shm->transit_fleet[k].occupancy_status = 0;
        shm->transit_fleet[k].total_trips = 0;
        shm->transit_fleet[k].journey_progress = 0.0;
    }

    memset(commuter_pids, 0, sizeof(commuter_pids));

    pipe(log_pipe);

    fcntl(log_pipe[0],
          F_SETFL,
          O_NONBLOCK);

    gtk_init(&argc, &argv);

    GtkWidget *window =
        gtk_window_new(GTK_WINDOW_TOPLEVEL);

    gtk_window_set_title(GTK_WINDOW(window),
                         "Advanced Autonomous Dispatch Center");

    gtk_window_set_default_size(GTK_WINDOW(window),
                                500,
                                600);

    g_signal_connect(window,
                     "destroy",
                     G_CALLBACK(cleanup_system),
                     NULL);

    GtkWidget *vbox =
        gtk_box_new(GTK_ORIENTATION_VERTICAL,
                    10);

    gtk_container_add(GTK_CONTAINER(window),
                      vbox);

    GtkWidget *hbox_controls =
        gtk_box_new(GTK_ORIENTATION_HORIZONTAL,
                    5);

    GtkWidget *btn_start =
        gtk_button_new_with_label("Start Dispatch");

    GtkWidget *btn_pause =
        gtk_button_new_with_label("Pause (SIGSTOP)");

    GtkWidget *btn_resume =
        gtk_button_new_with_label("Resume (SIGCONT)");

    g_signal_connect(btn_start,
                     "clicked",
                     G_CALLBACK(on_start_clicked),
                     NULL);

    g_signal_connect(btn_pause,
                     "clicked",
                     G_CALLBACK(on_pause_clicked),
                     NULL);

    g_signal_connect(btn_resume,
                     "clicked",
                     G_CALLBACK(on_resume_clicked),
                     NULL);

    gtk_box_pack_start(GTK_BOX(hbox_controls),
                       btn_start,
                       TRUE,
                       TRUE,
                       5);

    gtk_box_pack_start(GTK_BOX(hbox_controls),
                       btn_pause,
                       TRUE,
                       TRUE,
                       5);

    gtk_box_pack_start(GTK_BOX(hbox_controls),
                       btn_resume,
                       TRUE,
                       TRUE,
                       5);

    gtk_box_pack_start(GTK_BOX(vbox),
                       hbox_controls,
                       FALSE,
                       FALSE,
                       5);

    stats_label = gtk_label_new("");

    gtk_box_pack_start(GTK_BOX(vbox),
                       stats_label,
                       FALSE,
                       FALSE,
                       10);

    for(int i = 0; i < FLEET_CAPACITY; i++) {

        GtkWidget *car_box =
            gtk_box_new(GTK_ORIENTATION_VERTICAL,
                        2);

        fleet_labels[i] = gtk_label_new("");

        fleet_progress[i] = gtk_progress_bar_new();

        gtk_box_pack_start(GTK_BOX(car_box),
                           fleet_labels[i],
                           FALSE,
                           FALSE,
                           0);

        gtk_box_pack_start(GTK_BOX(car_box),
                           fleet_progress[i],
                           FALSE,
                           FALSE,
                           0);

        gtk_box_pack_start(GTK_BOX(vbox),
                           car_box,
                           FALSE,
                           FALSE,
                           5);
    }

    GtkWidget *scrolled_window =
        gtk_scrolled_window_new(NULL, NULL);

    gtk_widget_set_size_request(scrolled_window,
                                -1,
                                150);

    GtkWidget *text_view =
        gtk_text_view_new();

    gtk_text_view_set_editable(
        GTK_TEXT_VIEW(text_view),
        FALSE
    );

    log_buffer =
        gtk_text_view_get_buffer(
            GTK_TEXT_VIEW(text_view)
        );

    gtk_container_add(GTK_CONTAINER(scrolled_window),
                      text_view);

    gtk_box_pack_start(GTK_BOX(vbox),
                       scrolled_window,
                       TRUE,
                       TRUE,
                       5);

    g_timeout_add(16,
                  update_gui_state,
                  NULL);

    gtk_widget_show_all(window);

    gtk_main();

    return 0;
}
