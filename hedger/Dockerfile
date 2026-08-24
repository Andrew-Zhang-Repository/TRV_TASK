FROM python:3.11-slim

WORKDIR /app

COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

COPY hedger.py .

# Ensure Python prints directly to the console without buffering
ENV PYTHONUNBUFFERED=1

CMD ["python", "hedger.py"]
