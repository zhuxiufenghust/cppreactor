GCC=g++
CFLAGS= -g -Wall -DBOOST_LOG_DYN_LINK
INC:= -I../../include/ -I /usr/local/libevent/include/
LIB:=  -L/usr/local/libevent/lib/ -levent -lpthread -lboost_thread -lboost_program_options \
	 -lboost_log_setup	-lboost_log -lboost_regex  

SRC = $(wildcard *.cpp)
BIN_SRC = $(wildcard *_main.cpp)
COMM_SRC = $(filter-out $(BIN_SRC), $(SRC))
COMM_OBJ = $(patsubst  %.cpp, %.o, $(COMM_SRC))
BIN_OBJ = $(patsubst  %.cpp, %.o, $(BIN_SRC))
BIN_BIN = $(patsubst  %.o, %, $(BIN_OBJ))


all: $(BIN_BIN)

$(BIN_BIN):$(BIN_OBJ) $(COMM_OBJ)
	$(GCC) $(CFLAGS) -o $@ $@.o $(COMM_OBJ) $(LIB)

%.o:%.cpp
	$(GCC) $(CFLAGS) -c -o $@ $^ $(INC)
%.o:%.c
	$(GCC) $(CFLAGS) -c -o $@ $^ $(INC)

.PHONY:clean
clean:
	@echo $(COMM_OBJ)
	@echo $(BIN_BIN)
	rm -rf $(COMM_OBJ) $(BIN_OBJ) $(BIN_BIN)
