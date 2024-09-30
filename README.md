# Network Optimization
## Description
In this project, we aimed to set up a key value remote database. The clients can store the key:value pair in the server, and letter it can retrieve the value corresponds to the key from the serve. 
We will use RDMA to increase the speed of data transfer between clients and server. 
### Benchmark (Baseline)
We use a tcp socket to test what is the baseline speed of data transfer for different data size. 
### Optimization 
We use RDMA write to test what is the theoretical speed of transferring data when using rdma write. 
### LowLatencyProtocols
In practice, inorder to securely transfer data, we need to add control messages back and forth. We use the EAGER protocol to send small values (<4kb), and the RENDEZVOUS protocol to transfer larger value. And we tested the results for different data sizes. 
We also allow multiple clients to store and get data from server at the same time. 

## Conclusion
Using RDMA to speed up data transfer is indeed effective. Our results show that RDMA is much faster than TCP. 
And when transferring large data, it is better to use RENDEZVOUS protocol. 

TCP can reach theoretically 897.8 Mbits/sec

RDMA write reached theoretically 37057.8 Mbits/sec

With Rendezvous, in practice, we reached 239.3 Mbits/sec in secure key-value set and retrieval.


## How to Run?
Option 1:
1. Create a cmake-build directory under the root directory

   cd <project_root_directory>

   mkdir <cmake-build_directory>
2. run (Make sure you installed cmake):

   cmake -S <project_root_directory> -B <cmake-build_directory>

   cmake --build <cmake-build_directory>
   Option 2:
   Simple use gcc to compile server.c and client.c separately.

Option 3 (Recommended):  
Use the make file under each sub_directory: run make in terminal.

### Run
simple open two terminals and run two executables separately. To test between two school servers, you just need to log
into each server on each terminal, clone the git repo, and then you have the executable.



And then on one server run

./server

On the other server run

./client <first_server_name>

(Or in Low Latency Protocols)

./client <first_server_name> <client_input_file>

Notice that <first_server_name> can be servername like "mlx-stud-0x" or server ip like "132.65.164.10x"

