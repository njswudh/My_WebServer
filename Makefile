CXX      = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -g
LDFLAGS  = -lmariadb -lpthread

SRCS = main.cpp \
       M_http/M_http.cpp \
       M_log/M_log.cpp \
       M_sql_pool/M_sql_pool.cpp \
       M_timer/M_timer.cpp

OBJS   = $(SRCS:.cpp=.o)
HEADERS = $(wildcard *.h) $(wildcard */*.h)   # 改了头文件（比如 M_epoll_mode.h）也要重编
TARGET = server

$(TARGET): $(OBJS)
	$(CXX) $(OBJS) -o $(TARGET) $(LDFLAGS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OBJS): $(HEADERS)

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: clean
