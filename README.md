# LLM Job Server
Use multiple machines to prompt an LLM.

## Usage
```sh
# ./server input.txt output.txt # Defaults to port 8000
# ./server input.txt output.txt 8080 # Port 8080 is explicitly specified
```
The output consists of lines of the input file followed by lines produced by the LLM. Prompts and responses are separated by tabs.
