#include "Server.hpp"

int main() {
    Server server(8080, "www");
    server.run();
}
