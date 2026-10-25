### Prerequisites

```shell
# Python3
brew install python@3.12
# Sqlite3
brew install sqlite
# Ffmpeg
brew install ffmpeg
```

### AI Models

you need to provide your backend with an active Hugging Face token.

1. Go to Hugging Face and create a free account.
2. Search for meta-llama/Llama-3.2-3B-Instruct and click the button to Accept the License/Terms.
3. Go to your Hugging Face Settings > Access Tokens and create a new token (Read permissions are fine).
4. Open your backend's .env file and add the token to it:
    ```text
    ...
    HF_TOKEN=hf_your_huggingface_token_here
    ...
    ```

## Install

```shell
# Create the virtual environment (named 'venv')
rm -rf venv
python3.12 -m venv venv

# Activate the environment
source venv/bin/activate

# Install dependencies
pip install --upgrade pip
pip install -r requirements.txt
```

Run

```shell
# Activate the environment
source venv/bin/activate

# Run the server
python3.12 ingest_service.py --debug
python3.12 admin_service.py --debug
```

The ingest url is served at http://127.0.0.1:8443
The admin url is served at http://127.0.0.1:8080
