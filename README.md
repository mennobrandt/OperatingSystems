# _A3: Multi-Threaded Network Server for Pattern Analysis_
### 1. Building the program:
- Clear up existing files, with:
```
make clean
```
- Compile the program, with:
```
make
```

### 2. Running the Server:
- The format, is:
```
./assignment3 -l <port> -p "<pattern>"
``` 
- Here's an example, using port 1234 and the pattern "love":
```
./assignment3 -l 1234 -p "love"
```

### Running Clients:
- The format for a new client, is:
```
nc -q 0 localhost <port> < <file.txt>
```
- To use multiple clients, run the command in a seperate terminal window. Or in the background with `&`. 
- `-q 0` is used to close the connection, after the file has been sent. To close the connection without it, use `CTRL + C`. 
- Here's an example client, sending Romeo and Juliet to port 1234:
```
nc -q 0 localhost 1234 < books/romeojuliet.txt
```
- The following UTF-8 plaintext books (from Project Gutenberg) are stored under the `books/` directory:
```
alicewonderland.txt
metamorphosis.txt
mobydick.txt
romeojuliet.txt
frankenstein.txt
middlemarch.txt
prideandprejudice.txt
roomwithview.txt
```
