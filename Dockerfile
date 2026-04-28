FROM gcc:13

# Install Node.js
RUN apt-get update && apt-get install -y nodejs npm

WORKDIR /app

COPY . .

# Compile compiler
RUN g++ -std=c++17 "Compiler PJ 3"/*.cc -o compiler

# Move compiler to project folder
RUN mv compiler "Compiler PJ 3"/

WORKDIR /app/"Compiler PJ 3"

# Install node dependencies
RUN npm install

# Start server
CMD ["node", "server.js"]