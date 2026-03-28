CXX = g++
CXXFLAGS = -std=c++17 -O2 -Wall -fPIC
LIBS = -lavcodec -lavformat -lavutil -lswscale -lpthread

TARGET = r_plugin_frametap.so
SRC = plugin_frametap.cpp

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) -shared -o $(TARGET) $(SRC) $(LIBS)

clean:
	rm -f $(TARGET)
