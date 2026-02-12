CXX      = g++
CXXFLAGS = -std=c++17 -O2 -Wall -Wextra -INetwork -IThirdParty/enet/include

SRCS = main.cpp Network/enet_server_network.cpp Network/game_server.cpp
OBJS = $(SRCS:.cpp=.o)
TARGET = game_server

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(OBJS) -o $@ -LThirdParty/enet/lib -lenet -lpthread

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean
