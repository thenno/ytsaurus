package ru.yandex.yt.ytclient.rpc;

import java.util.concurrent.ScheduledExecutorService;

import ru.yandex.yt.ytclient.rpc.internal.RpcServiceClient;
import ru.yandex.yt.ytclient.rpc.internal.TokenAuthentication;

/**
 * Клиент через который можно делать запросы и получать на них ответы
 */
public interface RpcClient extends AutoCloseable {
    /**
     * Асинхронно закрывает rpc клиент
     */
    @Override
    void close();

    /**
     * Отправляет запрос, который можно отменить через возвращённый CompletableFuture
     * <p>
     * Сериализация запроса происходит в текущем потоке
     */
    RpcClientRequestControl send(RpcClient sender, RpcClientRequest request, RpcClientResponseHandler handler);

    default
    RpcClientRequestControl send(RpcClientRequest request, RpcClientResponseHandler handler)
    {
        return send(this, request, handler);
    }


    /**
     * Возвращает клиент с аутентификацией запросов по токену
     * @deprecated {@see #withTokenAuthentication}
     */
    @Deprecated
    default RpcClient withTokenAuthentication(String user, String token) {
        return withTokenAuthentication(new RpcCredentials(user, token));
    }

    default RpcClient withTokenAuthentication(RpcCredentials credentials) {
        return new TokenAuthentication(this, credentials);
    }

    /**
     * Возвращает клиент для сервиса с интерфейсом interfaceClass
     */
    default <T> T getService(Class<T> interfaceClass) {
        return RpcServiceClient.create(this, interfaceClass);
    }

    /**
     * Возвращает клиент для сервиса с интерфейсом interfaceClass
     */
    default <T> T getService(Class<T> interfaceClass, RpcOptions options) {
        return RpcServiceClient.create(this, interfaceClass, options);
    }

    String destinationName();

    ScheduledExecutorService executor();
}
