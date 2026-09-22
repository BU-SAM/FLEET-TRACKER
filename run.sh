#!/bin/bash
cd "$(dirname "$0")/backend"
pip install -r requirements.txt
python app.py
