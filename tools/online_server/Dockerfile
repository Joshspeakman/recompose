# The online server in a container. Fly.io builds this on its own machines (deploy.ps1,
# deploy.sh): nothing needs Docker here. On Fly a UDP service receives on the address
# fly-global-services, on the same port outside and in. Standard library only.
FROM python:3.13-slim
COPY server.py /app/server.py
USER nobody
EXPOSE 27630/udp
CMD ["python3", "-u", "/app/server.py", "--port", "27630", "--bind", "fly-global-services"]
