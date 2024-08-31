#include "utils.h"
//using namespace std;




/**
 * The function waits for a reply from the server and processes it when it arrives.
 * @param client_socket - The client's socket descriptor id.
 * @return 0 if the function succeeded and -1 otherwise.
 */
int receive_reply(int client_socket) {
    // Receive a reply from the server
    char buffer[server_reply_len];
    int bytes_received = recv(client_socket, buffer, server_reply_len, 0);

    if (bytes_received == -1) {
        cerr << "Error receiving reply." << endl;
        return -1;
    }
    return 0;
}

/**
 * The function sends messages to the server.
 * @param client_socket - The client's socket descriptor id.
 * @param message - The buffer containing the message to send.
 * @param size - The size of the messages to send.
 * @param num_messages_ind - An index, 0 or 1, specifying the type of messages (WarmUp=0, Real=1)
 * @return 0 if the function succeeded and -1 otherwise.
 */
int send_messages(int client_socket, char* message, int size, int num_messages_ind ){

    int message_count = 0;

    // loop until all the messages are sent
    while (message_count < num_messages[num_messages_ind]) {
        int bytes_received = 0, total=0;

        // for each message, loop (via "send" method) until all the bits in the message are sent
        while(total < size){
            bytes_received = send(client_socket, message + total, size - total, 0);
            if (bytes_received < 0)
            {
                cerr << "Error receiving message." << endl;
                return -1;
            }
            total+=bytes_received;
        }
        message_count++;
    }
    return 0;
}

/**
 * The function creates the client socket and connects it to the server, using the IP provided (given as an argument
 * to the program).
 * @param server_ip - The IP of the server.
 * @return The client's socket descriptor id if the function succeeded and -1 otherwise.
 */
int socket_creation(const char* server_ip){
    int client_socket;
    struct sockaddr_in server_addr;

    // create client socket
    client_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (client_socket == -1) {
        cerr << "Error creating socket." << endl;
        return -1;
    }

    // set up server address
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port_num);
    server_addr.sin_addr.s_addr = inet_addr(server_ip);

    // connect to server
    if (connect(client_socket, (struct sockaddr *) &server_addr, sizeof(server_addr)) < 0) {
        cerr << "Error connecting to server." << endl;
        return -1;
    }
    return client_socket;
}

/**
 * The function handles the process of the client. It sends messages of varying sizes to the server and calculates
 * the throughput each time (and inserts it into the output file).
 * @param argc - The number of arguments of the program.
 * @param argv - The arguments of the program
 * @return 0 if the function succeeded and -1 otherwise.
 */
int main(int argc, char const *argv[]) {
    // check if the right number of arguments is given
    if(argc != 2) {
        std::cerr << "Usage: client <server ip>" << std::endl;
        exit(1);
    }

    // open an output file

    std::ofstream out_file("output_" + to_string(warmup_messages) + "_" + to_string(real_messages) + "_4.txt");
    if(!out_file.is_open()){
        cerr << "Error opening output file." << endl;
        return -1;
    }

    // insert the first line (header) of the output file.
    out_file << header << endl;

    // loop through the message sizes and for each one, open up a socket with the server,
    // send messages (warm up and real) and calculate the throughput.
    for (int size : message_sizes) {
        int client_socket = socket_creation(argv[1]);
        if (client_socket == -1) {
            cerr << "Error creating socket." << endl;
            return -1;
        }
        char * message = new char[size];

        // send warmup messages
        if(send_messages(client_socket, message,size,warmup_messages_ind)==-1){
            return -1;
        }

        // receive reply to warm up from server
        if(receive_reply(client_socket)==-1){
            return -1;
        }


        // start timer
        auto start_time = chrono::system_clock::now();

        if(send_messages(client_socket, message,size, real_messages_ind)==-1){
            return -1;
        }
        delete[] message;

        // receive reply from server
        if(receive_reply(client_socket)==-1){
            return -1;
        }

        // stop timer and calculate throughput
        auto end_time = chrono::system_clock::now();
        chrono::duration<double> elapsed_seconds = end_time - start_time;
        long long msg_in_bytes = (long long) real_messages * size;
        double throughput = (double) msg_in_bytes / elapsed_seconds.count();
//        cout << "real_messages: " << real_messages << " messages per nano-second." << endl;
//        cout << "size: " << size << " messages per nano-second." << endl;
//        cout << "bits_in_byte: " << bits_in_byte << " messages per nano-second." << endl;
//        cout << "real_messages * size * bits_in_byte: " << msg_in_bytes << " messages per nano-second." << endl;
//        cout << "elapsed_seconds.count(): " << elapsed_seconds.count() << " messages per nano-second." << endl;

        // In order to print the throughput in units of bits per nanosecond, we normalize it by dividing with 10^9.
        double nano_throughput = (double)throughput / (bytes_in_KB * bytes_in_KB);
//        cout << "Duration: " << elapsed_seconds.count() << endl;
//
//        cout << "Throughput: " << throughput << " messages per nano-second." << endl;
//        cout << "nano_throughput: " << nano_throughput << " messages per nano-second." << endl;

        // close client socket
        close(client_socket);
        out_file << size <<"\t"<<nano_throughput<<"\tMB/S(MegaBytesPerSecond)" << endl;

    }
    // closes the output file
    out_file.close();
    return 0;
}