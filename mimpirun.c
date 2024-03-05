/**
 * This file is for implementation of mimpirun program.
 * */
#include "mimpi_common.h"
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include "channel.h"

//Returns pipe that has reading-end == first_free_fd_num, 
//writing-end == first_free_fd_num + 1
void create_pipe(int first_free_fd_num) {
    int fd[2];
    ASSERT_SYS_OK(channel(fd));
    //So that the dsc number in range [20, 1023]
    ASSERT_SYS_OK(dup2(fd[0], first_free_fd_num));
    ASSERT_SYS_OK(dup2(fd[1], first_free_fd_num + 1));
    //Close original fd_point_to_points.
    ASSERT_SYS_OK(close(fd[0]));
    ASSERT_SYS_OK(close(fd[1]));
}

int main(int argc, char *argv[]) {

    if (argc < 3) {
        fprintf(stderr, "Usage: %s n prog [args...]\n", argv[0]);
        return 1;
    }
    int n = 0;
    n = atoi(argv[1]);              // n - number of copies to run.

    //Creating pipes for point-to-point communication.
    int fd_point_to_point[n*n][2];
    int first_free_fd_num = 20;
    pid_t threads[n];

    for (int i = 0; i < n*n; i++) {
        create_pipe(first_free_fd_num);
        fd_point_to_point[i][0] = first_free_fd_num;
        fd_point_to_point[i][1] = first_free_fd_num + 1;
        first_free_fd_num += 2;
    }

    //Creating pipes for collective communication.
    int fd_collective_size = 6;
    int fd_collective[n][fd_collective_size];
    int recv_from_parent = 0,   recv_from_left_child = 2,   recv_from_right_child = 4,
        send_to_parent = 1,     send_to_left_child = 3,     send_to_right_child = 5;
    for (int i = 0; i < n; i++) {
        fd_collective[i][0] = 0; fd_collective[i][1] = 0; fd_collective[i][2] = 0;
        fd_collective[i][3] = 0; fd_collective[i][4] = 0; fd_collective[i][5] = 0;
    }
    
    for (int rank = 0; rank < n; rank++) {
        int parent_rank = (rank - 1) / 2;
        int left_child_rank = 2*rank + 1;
        int right_child_rank = 2*(rank + 1); 
        bool i_am_right_child = (rank % 2 == 0);

        //Set parent. 
        if (rank != 0) {
            create_pipe(first_free_fd_num);
            if (i_am_right_child) fd_collective[parent_rank][recv_from_right_child] = first_free_fd_num;
            else fd_collective[parent_rank][recv_from_left_child] = first_free_fd_num;
            fd_collective[rank][send_to_parent] = first_free_fd_num + 1;
            first_free_fd_num += 2;
        }
        //Set left child.
        if (left_child_rank < n) {
            create_pipe(first_free_fd_num);
            fd_collective[left_child_rank][recv_from_parent] = first_free_fd_num;
            fd_collective[rank][send_to_left_child] = first_free_fd_num + 1;
            first_free_fd_num += 2;
        }
        //Set right child.
        if (right_child_rank < n) {
            create_pipe(first_free_fd_num);
            fd_collective[right_child_rank][recv_from_parent] = first_free_fd_num;
            fd_collective[rank][send_to_right_child] = first_free_fd_num + 1;
            first_free_fd_num += 2;
        }
    }

    //Some necessary declarations mostly for str formatting.
    char formatted_str[STR_FORMAT_SIZE];
    char formatted_fd[STR_FORMAT_SIZE];
    int descriptor_no = -1;

    //Setting MIMPI_WORLD_SIZE.
    sprintf(formatted_str, "%d", n);
    setenv(MIMPI_WORLD_SIZE, formatted_str, 1);

    for (int rank = 0; rank < n; rank++) {

        pid_t pid;
        ASSERT_SYS_OK(pid = fork());
        threads[rank] = pid; 

        if (pid == 0) {

            //Setting MIMPI_RANK.
            sprintf(formatted_str, "%d", rank);
            setenv(MIMPI_WORLD_RANK, formatted_str, 1);

            //Descriptors lists.
            int* dsc_write = calloc(n, sizeof(int));
            int* dsc_read = calloc(n, sizeof(int));
    
            //Setting read descriptors, point-to-point communication.
            int idx_read = 0;
            for (int idx = 0 ; idx < n; idx++) {
                descriptor_no = fd_point_to_point[idx+(n*rank)][0];
                sprintf(formatted_str, "%s_%d", MIMPI_READ, idx);
                sprintf(formatted_fd, "%d", descriptor_no);
                setenv(formatted_str, formatted_fd, 1);
                dsc_read[idx_read] = descriptor_no;
                idx_read++;
            }

            //Setting write descriptors, point-to-point communication.
            int idx_write = 0;
            for (int idx = rank ; idx < n*n; idx+=n) {
                descriptor_no = fd_point_to_point[idx][1];
                sprintf(formatted_str, "%s_%d", MIMPI_WRITE, idx_write);
                sprintf(formatted_fd, "%d", descriptor_no);
                setenv(formatted_str, formatted_fd, 1);
                dsc_write[idx_write] = descriptor_no;
                idx_write++;
            }

            //Setting collective communication descriptors. 
            sprintf(formatted_str, "%d", fd_collective[rank][recv_from_parent]);
            setenv(MIMPI_RECV_PARENT, formatted_str, true);
            sprintf(formatted_str, "%d", fd_collective[rank][send_to_parent]);
            setenv(MIMPI_SEND_PARENT, formatted_str, true);
            sprintf(formatted_str, "%d", fd_collective[rank][recv_from_left_child]);
            setenv(MIMPI_RECV_L_CHILD, formatted_str, true);
            sprintf(formatted_str, "%d", fd_collective[rank][send_to_left_child]);
            setenv(MIMPI_SEND_L_CHILD, formatted_str, true);
            sprintf(formatted_str, "%d", fd_collective[rank][recv_from_right_child]);
            setenv(MIMPI_RECV_R_CHILD, formatted_str, true);
            sprintf(formatted_str, "%d", fd_collective[rank][send_to_right_child]);
            setenv(MIMPI_SEND_R_CHILD, formatted_str, true);

            //FIXME: improve. Closing unused point-to-point descriptors = n^2 (unfortunately, but n is small - MAX 16). 
            for (int i = 0; i < n * n; ++i) {
                int read_dsc_to_possibly_close = fd_point_to_point[i][0];
                int write_dsc_to_possibly_close = fd_point_to_point[i][1];
                bool close_read = true, close_write = true;

                for(int j = 0; j < n; j++) { 
                    if (dsc_read[j] == read_dsc_to_possibly_close) {
                        close_read = false;
                        break;
                    }
                }
                for(int j = 0; j < n; j++) {
                    if (dsc_write[j] == write_dsc_to_possibly_close) {
                        close_write = false;
                        break;
                    }
                }

                if (close_read) ASSERT_SYS_OK(close(read_dsc_to_possibly_close));
                if (close_write) ASSERT_SYS_OK(close(write_dsc_to_possibly_close));
            }

            //Closing unused collective descriptors
            for (int i = 0; i < n; i++) {
                if (i != rank) {
                    for (int j = 0; j < fd_collective_size; j++) {
                        if (fd_collective[i][j] != 0) ASSERT_SYS_OK(close(fd_collective[i][j]));                        
                    }
                }
            }

            free(dsc_write);
            free(dsc_read);

            if (execvp(argv[2], &argv[2]) < 0) {
                perror("execvp");
                exit(-1);
            }
            return 0;
        }
    }

    //Close rest of descriptors.
    for (int i=20; i<first_free_fd_num; i++)  {
        ASSERT_SYS_OK(close(i));
    }

    //Wait for n processes to end.
    for (int i = 0; i < n; i++) {
        ASSERT_SYS_OK(waitpid(threads[i], NULL, 0));
    }

    return 0;
}