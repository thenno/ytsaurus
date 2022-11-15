package ru.yandex.yt.ytclient.proxy.request;

import tech.ytsaurus.core.cypress.YPath;

import ru.yandex.lang.NonNullApi;

@NonNullApi
public class ListNode extends ru.yandex.yt.ytclient.request.ListNode.BuilderBase<ListNode>  {
    public ListNode(String path) {
        this(YPath.simple(path));
    }

    public ListNode() {
    }

    public ListNode(YPath path) {
        setPath(path);
    }

    public ListNode(ListNode listNode) {
        super(listNode);
    }

    public ListNode(ru.yandex.yt.ytclient.request.ListNode listNode) {
        super(listNode.toBuilder());
    }

    @Override
    protected ListNode self() {
        return this;
    }
}
