#!/bin/bash
cd ~/blackbox-ai 2>/dev/null || cd ~/blackbox-z 2>/dev/null || cd ~
# Try to find repo
if [ -d "blackbox.ai" ]; then
  cd blackbox.ai
elif [ -d "blackbox-z" ]; then
  cd blackbox-z
fi
pkill -f "uvicorn.*8000" 2>/dev/null
pkill -f "python.*server.py" 2>/dev/null
python3 -m venv .venv 2>/dev/null
source .venv/bin/activate
pip install --upgrade pip
pip install -r backend/requirements.txt 2>/dev/null || pip install fastapi uvicorn[standard] websockets pydantic
cd backend
nohup uvicorn server:app --host 0.0.0.0 --port 8000 > server.log 2>&1 &
echo $! > server.pid
echo "Server started on port 8000"
