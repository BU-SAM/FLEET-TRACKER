FROM python:3.11-slim

WORKDIR /app

COPY backend/requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

COPY backend/ ./backend/
COPY frontend/ ./frontend/

ENV HOST=0.0.0.0
ENV PORT=8000
ENV MAX_POINTS_PER_DEVICE=10000

EXPOSE 8000

CMD ["python", "backend/app.py"]
