#include "httpResponse.h"
#include "httpRequest.h"

HTTPSERVER_EXPORT QMimeDatabase HttpResponse::mimeDatabase;

HttpResponse::HttpResponse(HttpServerConfig *config, QObject *parent) : QObject(parent), config(config), status_(HttpStatus::None)
{
}

bool HttpResponse::isSending() const
{
    return !buffer.isEmpty();
}

bool HttpResponse::isValid() const
{
    return status_ != HttpStatus::None;
}

QString HttpResponse::version() const
{
    return version_;
}

HttpStatus HttpResponse::status() const
{
    return status_;
}

QByteArray HttpResponse::body() const
{
    return body_;
}

bool HttpResponse::header(QString key, QString *value) const
{
    auto it = headers.find(key);
    if (it == headers.end())
        return false;

    *value = it->second;
    return true;
}

bool HttpResponse::cookie(QString name, HttpCookie *cookie) const
{
    auto it = cookies.find(name);
    if (it == cookies.end())
        return false;

    *cookie = it->second;
    return true;
}

void HttpResponse::setStatus(HttpStatus status, bool finished_)
{
    status_ = status;

    if (finished_)
    {
        emit finished();
    }
}

void HttpResponse::setStatus(HttpStatus status, QByteArray body, QString contentType, bool finished_)
{
    status_ = status;
    body_ = body;

    // Auto-determine content type
    if (contentType.isEmpty())
        contentType = mimeDatabase.mimeTypeForData(body).name();

    // Note that the content type here must contain the charset in addition since it cannot be deduced from the body
    setHeader("Content-Type", contentType);

    if (finished_)
    {
        emit finished();
    }
}

void HttpResponse::setStatus(HttpStatus status, QJsonDocument body, bool finished_)
{
    status_ = status;
    body_ = body.toJson();

    setHeader("Content-Type", "application/json");

    if (finished_)
    {
        emit finished();
    }
}

void HttpResponse::setStatus(HttpStatus status, QString body, QString mimeType, bool finished_)
{
    status_ = status;
    body_ = body.toUtf8();

    setHeader("Content-Type", mimeType + "; charset=utf-8");

    if (finished_)
    {
        emit finished();
    }
}

void HttpResponse::setBody(QByteArray body)
{
    body_ = body;
}

void HttpResponse::setFinished()
{
    emit finished();
}

void HttpResponse::setError(HttpStatus status, QString errorMessage, bool closeConnection, bool finished_)
{
    auto it = config->errorDocumentMap.find(status);
    if (it != config->errorDocumentMap.end())
    {
        QFile file(it->second);
        if (file.open(QIODevice::ReadOnly))
        {
            QByteArray data = file.readAll();
            data.replace("${message}", errorMessage.toUtf8());
            data.replace("${statusCode}", QByteArray::number(int(status)));
            data.replace("${statusStr}", getHttpStatusStr(status).toUtf8());

            setStatus(status, data, "", finished_);

            if (config->errorDocumentCacheTime > 0)
                setHeader("Cache-Control", QString("max-age=%1").arg(config->errorDocumentCacheTime));
        }
        else
        {
            // Default to JSON object if we can't open the filename
            QJsonObject object;
            object["message"] = errorMessage;
            setStatus(status, QJsonDocument(object), finished_);
        }
    }
    else if (!errorMessage.isEmpty())
    {
        QJsonObject object;
        object["message"] = errorMessage;
        setStatus(status, QJsonDocument(object), finished_);
    }
    else
    {
        setStatus(status, finished_);
    }

    // If close connection is false, leave the connection header alone to default to what the client sent (or keep-alive if client sends nothing)
    if (closeConnection)
        headers["Connection"] = "close";
}

void HttpResponse::redirect(QUrl url, bool permanent)
{
    setStatus(permanent ? HttpStatus::PermanentRedirect : HttpStatus::TemporaryRedirect);
    setHeader("Location", url.toString());
}

void HttpResponse::redirect(QString url, bool permanent)
{
    setStatus(permanent ? HttpStatus::PermanentRedirect : HttpStatus::TemporaryRedirect);
    setHeader("Location", url);
}

void HttpResponse::compressBody(int compressionLevel)
{
    // Do nothing if there is no body
    if (body_.size() == 0)
        return;

    body_ = gzipCompress(body_, compressionLevel);
    setHeader("Content-Encoding", "gzip");
}

void HttpResponse::sendFile(QString filename, QString mimeType, QString charset, int len, int compressionLevel, QString attachmentFilename, int cacheTime)
{
    QFile file(filename);
    if (!file.open(QIODevice::ReadOnly))
    {
        if (config->verbosity >= HttpServerConfig::Verbose::Info)
            qInfo().noquote() << QString("Unable to open file to be sent (%1): %2").arg(filename).arg(file.errorString());

        return;
    }

    if (mimeType.isEmpty())
        mimeType = mimeDatabase.mimeTypeForFile(filename, QMimeDatabase::MatchExtension).name();

    sendFile(&file, mimeType, charset, len, compressionLevel, attachmentFilename, cacheTime);
}

void HttpResponse::sendFile(QIODevice *device, QString mimeType, QString charset, int len, int compressionLevel, QString attachmentFilename, int cacheTime)
{
    body_ = len != -1 ? device->read(len) : device->readAll();

    if (mimeType.isEmpty())
        mimeType = mimeDatabase.mimeTypeForData(device).name();

    setHeader("Content-Type", charset.isEmpty() ? mimeType : QString("%1; charset=%2").arg(mimeType).arg(charset));

    if (!attachmentFilename.isEmpty())
        setHeader("Content-Disposition", QString("attachment; filename=\"%1\"").arg(attachmentFilename));

    if (cacheTime > 0)
        setHeader("Cache-Control", QString("max-age=%1").arg(cacheTime));

    if (compressionLevel >= -1)
        compressBody(compressionLevel);
}

void HttpResponse::setCookie(HttpCookie &cookie)
{
    // Check if the cookie exists first
    if (cookies.find(cookie.name) != cookies.end())
    {
        if (config->verbosity >= HttpServerConfig::Verbose::Warning)
            qWarning().noquote() << QString("HTTP response cannot have two cookies with the same name: %1").arg(cookie.name);

        return;
    }

    // Insert the cookie
    cookies[cookie.name] = cookie;
}

void HttpResponse::setHeader(QString name, QString value, bool encode)
{
    headers[name] = encode ? QUrl::toPercentEncoding(value) : value;
}

void HttpResponse::setHeader(QString name, QDateTime value)
{
    headers[name] = value.toString(Qt::RFC2822Date);
}

void HttpResponse::setHeader(QString name, int value)
{
    headers[name] = QString::number(value);
}

void HttpResponse::setupFromRequest(HttpRequest *request)
{
    // If no connection is specified in the response, use the value from the request or default to keep-alive
    if (headers.find("Connection") == headers.end())
        headers["Connection"] = request ? request->headerDefault("Connection", QString("keep-alive")) : "keep-alive";

    if (status_ == HttpStatus::MethodNotAllowed && request)
    {
        // Combine the allowed methods into one string delineated by commas
        headers["Allow"] = std::accumulate(request->allowedMethods.begin(), request->allowedMethods.end(), QString(""), [](const QString &str1, const QString &str2) {
            return str1.isEmpty() ? str2 : str1 + ", " + str2;
        });
    }
}

void HttpResponse::prepareToSend()
{
    headers["Content-Length"] = QString::number(body_.size());

    // If the connection is keep-alive, then attach the keep alive timeout value
    if (headers["Connection"].contains("keep-alive", Qt::CaseInsensitive))
        headers["Keep-Alive"] = "timeout=" + QString::number(config->keepAliveTimeout);

    writeIndex = 0;
    buffer.clear();

    // Status line
    buffer += version_;
    buffer += ' ';
    buffer += QString::number((int)status_);
    buffer += ' ';
    buffer += getHttpStatusStr(status_);
    buffer += "\r\n";

    // Headers
    for (auto &keyValue : headers)
    {
        buffer += keyValue.first;
        buffer += ": ";
        buffer += keyValue.second;
        buffer += "\r\n";
    }

    // Cookies
    for (auto &keyValue : cookies)
    {
        buffer += "Set-Cookie: ";
        buffer += keyValue.second.toByteArray();
        buffer += "\r\n";
    }

    // Empty line signifies end of headers
    buffer += "\r\n";

    // Body
    buffer += body_;
}

bool HttpResponse::writeChunk(QTcpSocket *socket)
{
    int bytesWritten = socket->write(buffer.mid(writeIndex));
    if (bytesWritten == -1)
    {
        // Force close the socket and say we're done
        socket->close();
        return true;
    }

    // Increment the bytes written, if we wrote the entire buffer, return true, otherwise return false
    writeIndex += bytesWritten;
    if (writeIndex >= buffer.size() - 1)
        return true;

    return false;
}