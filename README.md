## How to Run?
## Benchmark
### Ip address
If you want to perform the experiment on local server, then set the ip address in server.c to 127.0.0.1, and pass 127.0.0.1 as argument for client.
If you want to perform the experiment between two computers, then set the ip address in server.c to the ip address of the server computer, and pass the ip as argument for client.

### Compilation
Option 1: 
1. Create a cmake-build directory under the root directory

   cd <project_root_directory>
   
   mkdir <cmake-build_directory>
3. run (Make sure you installed cmake):

   cmake -S <project_root_directory> -B <cmake-build_directory>
   
   cmake --build <cmake-build_directory>
Option 2:
Simple use gcc to compile server.c and client.c separately.

### Run
simple open two terminals and run two executables separately. To test etween two school servers, you just need to log into each server on each terminal, clone the git repo, and the you have the executable. 
 
To run on student server: simply 
