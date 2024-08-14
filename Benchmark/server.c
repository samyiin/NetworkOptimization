//
// Created by hsiny on 6/3/24.
//
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

#define SERVER_IP "132.65.164.101"
//#define SERVER_IP "127.0.0.1"

// "127.0.0.1": loopback address: only listen to clients on this machine
// INADDR_ANY: 0.0.0.0: to bind to all available interface
// "132.65.164.103": mlx-stud-03 server's public ip. 
#define PORT 8080
#define END_MESSAGE 'z' // this is a char, not a string as in Client.c, this way it's easier for comparason
#define RECEIVE_MESSAGE "R"
#define BUFFER_SIZE 2*1048576

int main() {
    int server_fd, new_socket;
    struct sockaddr_in address;
    int opt = 1;

    // AF_INET means it's ipv4, SOCK_STREAM means it's TCP, protocal=0 means automatically select protocal.
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }

    // set socket options: server_fd: the file descriptor of the server socket
    // protocal level: SOL_SOCKET: all the options is applied to this socket, not a specific protocol in this socket
    // option name: SO_REUSEADDR: allows multiple sockets to bind to the same address and port
    //     There is a slight difference between SO_REUSEADDR and SO_REUSEPORT, see documentation. Using together?
    // opt value: means value of "SO_REUSEADDR | SO_REUSEPORT" is 1. This could be pointer to a complex struct as value
    // since we need to pass optval as pointer, we need the size of it so that C can know where does it's content end.
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
        perror("setsockopt");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    // specify is it ip4 or ip6 (sin - socket internet, s - struct)
    address.sin_family = AF_INET;
    // Convert IPv4 addresses from text to binary form (pton: presentation to network)
    // AF_INET specifies that the address family is IPv4.
    // src: server_ip: is the string representation of the IP address.
    // dst: serv_addr.sin_addr: A pointer to a buffer where the function will store the binary representation of the
    //       IP address. If the conversion is successful, "dest".s_addr will contain the binary representation of the
    //       IP address. (It will store in the s_addr attribute of it).
    // Legacy: address.sin_addr.s_addr = inet_addr(server_ip)
    if (inet_pton(AF_INET, SERVER_IP, &address.sin_addr) <= 0) {
        printf("\nInvalid address/ Address not supported \n");
        return -1;
    }
    // set port number
    address.sin_port = htons(PORT);

    // bind the server to the above address, above port (since it's pointer, we need size)
    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("bind failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    // mark a socket as a passive socket,
    // that is, as a socket that will be used to accept incoming connection requests using the accept function
    // backlog = 3: maximum number of pending connections that can be queued up before the kernel starts to refuse new
    // connection attempts
    // so all the incomming connections will be put into a queue. Later accept will take a connection from the queue.
    if (listen(server_fd, 1) < 0) {
        perror("listen");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    // Sam's note for accpet()
    // accept(server_fd,...) is server.accept in oop
    // it will take one connection from the queue above, and create a new socket for that connection. If there is no
    // connection in the queue, then it will block the process. If the socket is labeled (somehow) "no-blcok" then see
    // documentation... Why create a new socket? so that the original socket can keep listening, and the new socket is
    // not labelled as listening, it can send and read message from the client (it's a different kinds of socket
    // already, Definitely by distinguishing socket and connected socket things become easy to understand.).
    // https://stackoverflow.com/questions/65912861/why-does-accept-create-a-new-socket
    // https://stackoverflow.com/questions/50743329/what-does-it-mean-that-accept-creates-a-new-socket
    // besides from server_fd, it also takes an address pointer (and it's size since it's a pointer)
    // this points to an "empty" address struct, and later accept function will fill in the client address into the
    // struct.
    // This is like the "server version of connect". So later if this new socket wants to send message to the client
    // socket, the os would know where to send to.

    // accept a new connection from client: see notes above.
    struct sockaddr_in client_addr;
    int client_addrlen = sizeof(client_addr);
    if ((new_socket = accept(server_fd, (struct sockaddr *)&client_addr, (socklen_t*)&client_addrlen)) < 0) {
        perror("accept");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    // each iter of the while is an experiment. The experiment starts with client sendig messages 'A', and ends when the
    // client send 'z'.
    while(1) {
        // The read call will block and wait for the client to send data. ( in a block TCP socket)
        // This means the server will pause execution at the read call until at least one byte of data is available.
        int bytes_read;
        char buffer[BUFFER_SIZE] = {0};

        while (1) {
            // if the client close it's socket, it will send FIN to this new_socket, so recv() would know and return 0.
            // it will not block the process from this point on.
            // As long as the clinet socket is still alive, the new_socket will keep listening, recv will keep blocking.
            // the recv() will always block until new message arrives. As long as the client didn't close, the recv()
            // will stay there.
            // how does recv() know a new message arrives? The process was in block mode. when new message arrived, the
            // OS will wake up this process by unblock it. (From NIC card to OS)2
            bytes_read = recv(new_socket, buffer, BUFFER_SIZE, 0);
            if (bytes_read <= 0) {
                // printf("Probably Client socket is closed\n");
                // printf("Closing all sockets in Server...\n");
                // close both sockets
                close(new_socket);
                close(server_fd);
                return 0;
            }
            // check if the last byte of the current buffer is the end: to determine the end of messages
            if (buffer[bytes_read -1] == END_MESSAGE){break;}
        }

        // send a message to the client to indicate that process is completed
        send(new_socket, RECEIVE_MESSAGE, 2,0);

        // close this socket: Becuase the read() will only block the process in the first time, afterwards, even if we
        // clear the buffer, set all value to 0, it will not block process anymore, it will keep reading from the
        // buffer (bytes read will be 0, and keep looping).
        // printf("Finished one Trail.\n");
    }

}



