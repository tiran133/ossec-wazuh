/* Copyright (C) 2009 Trend Micro Inc.
 * All right reserved.
 *
 * This program is a free software; you can redistribute it
 * and/or modify it under the terms of the GNU General Public
 * License (version 2) as published by the FSF - Free Software
 * Foundation
 */

#ifdef WIN32

#include "shared.h"
#include "os_execd/execd.h"
#include "os_crypto/md5/md5_op.h"
#include "os_net/os_net.h"
#include "agentd.h"


/* Receive events from the server */
void *receiver_thread(__attribute__((unused)) void *none)
{
    ssize_t recv_b;
    netsize_t length;
    int reads;
    char file[OS_SIZE_1024 + 1];
    char buffer[OS_MAXSTR + 1];

    char cleartext[OS_MAXSTR + 1];
    char *tmp_msg;

    char file_sum[34];

    fd_set fdset;
    struct timeval selecttime;

    FILE *fp;

    /* Set FP to null before starting */
    fp = NULL;

    memset(cleartext, '\0', OS_MAXSTR + 1);
    memset(buffer, '\0', OS_MAXSTR + 1);
    memset(file, '\0', OS_SIZE_1024 + 1);
    memset(file_sum, '\0', 34);

    while (1) {
        /* sock must be set */
        if (agt->sock == -1) {
            sleep(5);
            continue;
        }

        FD_ZERO(&fdset);
        FD_SET(agt->sock, &fdset);

        /* Wait for 30 seconds */
        selecttime.tv_sec = 30;
        selecttime.tv_usec = 0;

        /* Wait with a timeout for any descriptor */
        recv_b = select(0, &fdset, NULL, NULL, &selecttime);
        if (recv_b == -1) {
            merror(SELECT_ERROR, ARGV0, errno, strerror(errno));
            sleep(30);
            continue;
        } else if (recv_b == 0) {
            continue;
        }

        reads = 0;

        /* Read until no more messages are available */
        while (1) {
            if (agt->protocol == TCP_PROTO) {
                /* Only one read per call */
                if (reads++) {
                    break;
                }

                recv_b = recv(agt->sock, (char*)&length, sizeof(length), MSG_WAITALL);

                if (recv_b <= 0) {
                    merror(LOST_ERROR, ARGV0);
                    start_agent(0);
                    break;
                }

                recv_b = recv(agt->sock, buffer, length, MSG_WAITALL);

                if (recv_b != length) {
                    merror(RECV_ERROR, ARGV0);
                    break;
                }
            } else {
                recv_b = recv(agt->sock, buffer, OS_SIZE_1024, 0);

                if (recv_b <= 0) {
                    break;
                }
            }

            /* Id of zero -- only one key allowed */
            tmp_msg = ReadSecMSG(&keys, buffer, cleartext, 0, recv_b - 1);
            if (tmp_msg == NULL) {
                merror(MSG_ERROR, ARGV0, agt->rip[agt->rip_id]);
                continue;
            }

            /* Check for commands */
            if (IsValidHeader(tmp_msg)) {
                /* This is the only thread that modifies it */
                available_server = (int)time(NULL);

                /* Run timeout commands */
                if (agt->execdq >= 0) {
                    WinTimeoutRun(available_server);
                }

                /* If it is an active response message */
                if (strncmp(tmp_msg, EXECD_HEADER, strlen(EXECD_HEADER)) == 0) {
                    tmp_msg += strlen(EXECD_HEADER);

                    /* Run on Windows */
                    if (agt->execdq >= 0) {
                        WinExecdRun(tmp_msg);
                    }

                    continue;
                }

                /* Restart syscheck */
                else if (strcmp(tmp_msg, HC_SK_RESTART) == 0) {
                    os_set_restart_syscheck();
                    continue;
                }

                /* Ack from server */
                else if (strcmp(tmp_msg, HC_ACK) == 0) {
                    continue;
                }

                /* Close any open file pointer if it was being written to */
                if (fp) {
                    fclose(fp);
                    fp = NULL;
                }

                /* File update message */
                if (strncmp(tmp_msg, FILE_UPDATE_HEADER,
                            strlen(FILE_UPDATE_HEADER)) == 0) {
                    char *validate_file;
                    tmp_msg += strlen(FILE_UPDATE_HEADER);

                    /* Going to after the file sum */
                    validate_file = strchr(tmp_msg, ' ');
                    if (!validate_file) {
                        continue;
                    }

                    *validate_file = '\0';

                    /* Copy the file sum */
                    strncpy(file_sum, tmp_msg, 33);

                    /* Set tmp_msg to the beginning of the file name */
                    validate_file++;
                    tmp_msg = validate_file;

                    if ((validate_file = strchr(tmp_msg, '\n')) != NULL) {
                        *validate_file = '\0';
                    }

                    while ((validate_file = strchr(tmp_msg, '/')) != NULL) {
                        *validate_file = '-';
                    }

                    if (tmp_msg[0] == '.') {
                        tmp_msg[0] = '-';
                    }

                    snprintf(file, OS_SIZE_1024, "%s/%s",
                             SHAREDCFG_DIR,
                             tmp_msg);

                    fp = fopen(file, "w");
                    if (!fp) {
                        merror(FOPEN_ERROR, ARGV0, file, errno, strerror(errno));
                    }
                }

                else if (strncmp(tmp_msg, FILE_CLOSE_HEADER,
                                 strlen(FILE_CLOSE_HEADER)) == 0) {
                    /* No error */
                    os_md5 currently_md5;

                    /* Close for the rename to work */
                    if (fp) {
                        fclose(fp);
                        fp = NULL;
                    }

                    if (file[0] == '\0') {
                        /* Nothing to be done */
                    }

                    else if (OS_MD5_File(file, currently_md5, OS_TEXT) < 0) {
                        /* Remove file */
                        unlink(file);
                        file[0] = '\0';
                    } else {
                        if (strcmp(currently_md5, file_sum) != 0) {
                            debug1("%s: Failed md5 for: %s -- deleting.",
                                   ARGV0, file);
                            unlink(file);
                        } else {
                            char *final_file;

                            /* Rename the file to its original name */
                            final_file = strrchr(file, '/');
                            if (final_file) {
                                if (strcmp(final_file + 1, SHAREDCFG_FILENAME) == 0) {
                                    UnmergeFiles(file, SHAREDCFG_DIR);
                                }
                            } else {
                                unlink(file);
                            }
                        }

                        file[0] = '\0';
                    }
                }

                else {
                    merror("%s: WARN: Unknown message received from server.", ARGV0);
                }
            }

            else if (fp) {
                available_server = (int)time(NULL);
                fprintf(fp, "%s", tmp_msg);
            }

            else {
                merror("%s: WARN: Unknown message received. No action defined.",
                       ARGV0);
            }
        }
    }

    /* Clean up */
    if (fp) {
        fclose(fp);
        if (file[0] != '\0') {
            unlink(file);
        }
    }

    return (NULL);
}

#endif /* WIN32 */

