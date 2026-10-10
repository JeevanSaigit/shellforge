#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <pty.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>

#define MAX_SESSIONS 10
#define BUFFER_SIZE 4096

typedef struct {
    int id;
    pid_t pid;
    int master_fd;
    int active;
} Session;

Session sessions[MAX_SESSIONS];
int session_count = 0;


/* Find a session by ID */
Session *find_session(int id)
{
    for (int i = 0; i < session_count; i++) {
        if (sessions[i].id == id) {
            return &sessions[i];
        }
    }

    return NULL;
}


/* Create a new PTY session */
void create_session(void)
{
    if (session_count >= MAX_SESSIONS) {
        printf("Maximum number of sessions reached.\n");
        return;
    }

    int master_fd;
    pid_t pid = forkpty(&master_fd, NULL, NULL, NULL);

    if (pid == -1) {
        perror("forkpty");
        return;
    }

    if (pid == 0) {
        /* Child process */
        execl("/bin/bash",
              "bash",
              "--noprofile",
              "--norc",
              NULL);

        perror("execl");
        exit(EXIT_FAILURE);
    }

    /* Parent process */
    Session *session = &sessions[session_count];

    session->id = session_count + 1;
    session->pid = pid;
    session->master_fd = master_fd;
    session->active = 1;

    session_count++;

    printf("Session %d created. PID = %d\n",
           session->id,
           session->pid);
}


/* List all sessions */
void list_sessions(void)
{
    printf("\n");
    printf("ID\tPID\tSTATUS\n");
    printf("-------------------------\n");

    for (int i = 0; i < session_count; i++) {
        printf("%d\t%d\t%s\n",
               sessions[i].id,
               sessions[i].pid,
               sessions[i].active ? "ACTIVE" : "INACTIVE");
    }

    printf("\n");
}


/* Attach to a session */
void attach_session(int id)
{
    Session *session = find_session(id);

    if (session == NULL) {
        printf("Session %d not found.\n", id);
        return;
    }

    if (!session->active) {
        printf("Session %d is inactive.\n", id);
        return;
    }

    printf("Attaching to session %d...\n", id);
    printf("Press Ctrl+] to detach from the session.\n");

    struct termios original_terminal;
    struct termios raw_terminal;

    if (tcgetattr(STDIN_FILENO, &original_terminal) == -1) {
        perror("tcgetattr");
        return;
    }

    raw_terminal = original_terminal;
    cfmakeraw(&raw_terminal);

    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw_terminal) == -1) {
        perror("tcsetattr");
        return;
    }

    char buffer[BUFFER_SIZE];

    while (session->active) {

        fd_set readfds;

        FD_ZERO(&readfds);

        FD_SET(STDIN_FILENO, &readfds);
        FD_SET(session->master_fd, &readfds);

        int max_fd = STDIN_FILENO > session->master_fd
                         ? STDIN_FILENO
                         : session->master_fd;

        int result = select(max_fd + 1,
                            &readfds,
                            NULL,
                            NULL,
                            NULL);

        if (result == -1) {
            if (errno == EINTR) {
                continue;
            }

            perror("select");
            break;
        }


        /* Keyboard input */
        if (FD_ISSET(STDIN_FILENO, &readfds)) {

            ssize_t n = read(STDIN_FILENO,
                             buffer,
                             sizeof(buffer));

            if (n <= 0) {
                break;
            }

            /*
             * Ctrl+] = ASCII 29
             * Used to detach without terminating Bash.
             */
            if (n == 1 && buffer[0] == 0x1D) {
                break;
            }

            if (write(session->master_fd,
                      buffer,
                      n) == -1) {

                if (errno == EIO) {
                    break;
                }

                perror("write");
                break;
            }
        }


        /* Output from PTY */
        if (FD_ISSET(session->master_fd, &readfds)) {

            ssize_t n = read(session->master_fd,
                             buffer,
                             sizeof(buffer));

            if (n <= 0) {
                break;
            }

            if (write(STDOUT_FILENO,
                      buffer,
                      n) == -1) {
                break;
            }
        }
    }

    tcsetattr(STDIN_FILENO,
              TCSANOW,
              &original_terminal);

    printf("\nDetached from session %d.\n", id);
}


/* Terminate a session */
void kill_session(int id)
{
    Session *session = find_session(id);

    if (session == NULL) {
        printf("Session %d not found.\n", id);
        return;
    }

    if (!session->active) {
        printf("Session %d is already inactive.\n", id);
        return;
    }

    printf("Terminating session %d...\n", id);

    /*
     * Send SIGTERM to the Bash process.
     */
    if (kill(session->pid, SIGTERM) == -1) {

        if (errno == ESRCH) {
            printf("Process %d is already terminated.\n",
                   session->pid);
        } else {
            perror("kill");
            return;
        }
    }

    /*
     * Wait for a short period without blocking forever.
     */
    int terminated = 0;

    for (int i = 0; i < 20; i++) {

        pid_t result = waitpid(session->pid,
                               NULL,
                               WNOHANG);

        if (result == session->pid) {
            terminated = 1;
            break;
        }

        if (result == -1) {

            if (errno == ECHILD) {
                terminated = 1;
                break;
            }

            perror("waitpid");
            break;
        }

        usleep(100000);
    }

    /*
     * If the process did not terminate,
     * force termination using SIGKILL.
     */
    if (!terminated) {

        if (kill(session->pid, SIGKILL) == -1) {

            if (errno != ESRCH) {
                perror("SIGKILL");
            }

        } else {
            waitpid(session->pid, NULL, 0);
        }
    }

    close(session->master_fd);

    session->master_fd = -1;
    session->active = 0;

    printf("Session %d terminated.\n", id);
}


/* Check and clean up terminated child processes */
void cleanup_sessions(void)
{
    for (int i = 0; i < session_count; i++) {

        if (!sessions[i].active) {
            continue;
        }

        int status;

        pid_t result = waitpid(sessions[i].pid,
                               &status,
                               WNOHANG);

        if (result == sessions[i].pid) {

            close(sessions[i].master_fd);

            sessions[i].master_fd = -1;
            sessions[i].active = 0;

            printf("\nSession %d has terminated.\n",
                   sessions[i].id);
        }
    }
}


/* Main command interface */
int main(void)
{
    char command[BUFFER_SIZE];

    printf("=================================\n");
    printf("       PtyMux Terminal Mux       \n");
    printf("=================================\n");

    printf("Commands:\n");
    printf("  create          Create a session\n");
    printf("  list            List sessions\n");
    printf("  attach <id>     Attach to session\n");
    printf("  kill <id>       Terminate session\n");
    printf("  exit            Exit PtyMux\n");

    while (1) {

        cleanup_sessions();

        printf("ptymux> ");
        fflush(stdout);

        if (fgets(command,
                  sizeof(command),
                  stdin) == NULL) {
            break;
        }

        command[strcspn(command, "\n")] = '\0';


        /* Create */
        if (strcmp(command, "create") == 0) {

            create_session();
        }


        /* List */
        else if (strcmp(command, "list") == 0) {

            list_sessions();
        }


        /* Attach */
        else if (strncmp(command,
                         "attach ",
                         7) == 0) {

            int id = atoi(command + 7);

            attach_session(id);
        }


        /* Kill */
        else if (strncmp(command,
                         "kill ",
                         5) == 0) {

            int id = atoi(command + 5);

            kill_session(id);
        }


        /* Exit PtyMux */
        else if (strcmp(command, "exit") == 0) {

            printf("Exiting PtyMux...\n");

            for (int i = 0; i < session_count; i++) {

                if (sessions[i].active) {

                    kill(sessions[i].pid, SIGTERM);

                    waitpid(sessions[i].pid,
                            NULL,
                            WNOHANG);

                    close(sessions[i].master_fd);

                    sessions[i].active = 0;
                }
            }

            break;
        }


        /* Empty command */
        else if (strlen(command) == 0) {
            continue;
        }


        /* Unknown command */
        else {

            printf("Unknown command: %s\n",
                   command);

            printf("Available commands: "
                   "create, list, attach <id>, "
                   "kill <id>, exit\n");
        }
    }

    return 0;
}
