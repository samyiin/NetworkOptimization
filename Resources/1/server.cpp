
#include "utils.h"

//using namespace std;

/**
 * The function handles the process of the server. It opens a socket and waits for a connection with a client.
 * After each connection (for each message size), it processes the messages and sends a reply afterwards. At the end,
 * The server socket is closed.
 * @param argc - The number of arguments of the program.
 * @param argv - The arguments of the program
 * @return 0 if the function succeeded and -1 otherwise.
 */
int main(int argc, char const *argv[])
{
    int server_socket, client_socket;

    struct sockaddr_in server_addr;

    int message_count;

    // create server socket
    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket == -1)
    {
        cerr << "Error creating socket." << endl;
        return -1;
    }

    // find the ip of the host according to its name
    char hostname[1024];
    struct in_addr addr;
    if(gethostname(hostname, 1024)==-1){
        cerr << "Error getting hostname." << endl;
        return -1;
    }
    struct hostent* host = gethostbyname(hostname);
    if(host == NULL){
        cerr << "Error getting host." << endl;
        return -1;
    }

    // Convert the IP address to a string
    memcpy(&addr, host->h_addr_list[0], sizeof(struct in_addr));
    char* ip_address = inet_ntoa(addr);
    cout << "ip address: " << ip_address << endl;

    server_addr.sin_addr.s_addr = inet_addr(ip_address);

    // bind the socket to an IP address and port
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port_num);

    if (bind(server_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
    {
        cerr << "Error binding socket." << endl;
        return -1;
    }

    // listen for incoming connections
    listen(server_socket, 1);

    for (int message_size : message_sizes) {
        // accept incoming connections and create a new socket for each client connection

        client_socket = accept(server_socket, NULL,NULL);

        message_count = 0;

        // receive messages from client socket
        char* buffer = new char[message_size];


        // loop through the warm_up messages first

        // loop until all the messages are received

        while (message_count < warmup_messages)
        {
            int bytes_received = 0, total=0;

            // for each message, loop (via "send" method) until all the bits in the message are received
            while(total < message_size){
                bytes_received = recv(client_socket, buffer+total, message_size-total, 0);
                if (bytes_received < 0)
                {
                    cerr << "Error receiving message." << endl;
                    return -1;
                }
                total+=bytes_received;
            }
            message_count++;
        }

        // send reply to client to the Warm Up Messages
        int bytes_sent = send(client_socket, reply_message, strlen(reply_message), 0);
        if (bytes_sent == -1)
        {
            cerr << "Error sending message." << endl;
            return -1;
        }

        while (message_count < (real_messages + warmup_messages))
        {
            int bytes_received = 0, total=0;

            // for each message, loop (via "send" method) until all the bits in the message are received
            while(total < message_size){
                bytes_received = recv(client_socket, buffer+total, message_size-total, 0);
                if (bytes_received < 0)
                {
                    cerr << "Error receiving message." << endl;
                    return -1;
                }
                total+=bytes_received;
            }
            message_count++;
        }
        delete[] buffer;

        // send reply to client
        bytes_sent = send(client_socket, reply_message, strlen(reply_message), 0);
        if (bytes_sent == -1)
        {
            cerr << "Error sending message." << endl;
            return -1;
        }
    }
    // close the server socket
    close(server_socket);
    return 0;
}