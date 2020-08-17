package integration

import (
	"context"
	"testing"
	"time"

	"github.com/stretchr/testify/require"

	"a.yandex-team.ru/yt/go/ypath"
	"a.yandex-team.ru/yt/go/yt"
	"a.yandex-team.ru/yt/go/yttest"
)

func TestOperation(t *testing.T) {
	t.Parallel()

	env, cancel := yttest.NewEnv(t)
	defer cancel()

	ctx, cancel := context.WithTimeout(context.Background(), time.Minute)
	defer cancel()

	inTable := tmpPath()
	outTable := tmpPath()

	for _, p := range []ypath.Path{inTable, outTable} {
		_, err := env.YT.CreateNode(ctx, p, yt.NodeTable, nil)
		require.NoError(t, err)
	}

	w, err := env.YT.WriteTable(ctx, inTable, nil)
	require.NoError(t, err)
	require.NoError(t, w.Write(map[string]interface{}{"a": int64(1)}))
	require.NoError(t, w.Commit())

	spec := map[string]interface{}{
		"input_table_paths":  []ypath.Path{inTable},
		"output_table_paths": []ypath.Path{outTable},
		"mapper": map[string]interface{}{
			"input_format":  "yson",
			"output_format": "yson",
			"command":       "cat -",
		},
	}

	opID, err := env.YT.StartOperation(ctx, yt.OperationMap, spec, nil)
	require.NoError(t, err)

	for {
		time.Sleep(time.Second)

		status, err := env.YT.GetOperation(ctx, opID, nil)
		require.NoError(t, err)

		if status.State == yt.StateCompleted {
			break
		}
	}

	r, err := env.YT.ReadTable(ctx, outTable, nil)
	require.NoError(t, err)
	defer func() { _ = r.Close() }()

	var row interface{}
	require.True(t, r.Next())
	require.NoError(t, r.Scan(&row))
	require.Equal(t, map[string]interface{}{"a": int64(1)}, row)

	require.False(t, r.Next())
	require.NoError(t, r.Err())
}

func TestOperationWithStderr(t *testing.T) {
	t.Parallel()

	env, cancel := yttest.NewEnv(t)
	defer cancel()

	ctx, cancel := context.WithTimeout(context.Background(), time.Minute)
	defer cancel()

	inTable := tmpPath()
	outTable := tmpPath()

	for _, p := range []ypath.Path{inTable, outTable} {
		_, err := env.YT.CreateNode(ctx, p, yt.NodeTable, nil)
		require.NoError(t, err)
	}

	w, err := env.YT.WriteTable(ctx, inTable, nil)
	require.NoError(t, err)
	require.NoError(t, w.Write(map[string]interface{}{"a": int64(1)}))
	require.NoError(t, w.Commit())

	spec := map[string]interface{}{
		"input_table_paths":  []ypath.Path{inTable},
		"output_table_paths": []ypath.Path{outTable},
		"mapper": map[string]interface{}{
			"input_format":  "yson",
			"output_format": "yson",
			"command":       "echo hello >> /dev/stderr",
		},
	}

	opID, err := env.YT.StartOperation(ctx, yt.OperationMap, spec, nil)
	require.NoError(t, err)
	for {
		time.Sleep(time.Second)
		status, err := env.YT.GetOperation(ctx, opID, nil)
		require.NoError(t, err)
		if status.State == yt.StateCompleted {
			break
		}
	}
	jobs, err := env.YT.ListJobs(ctx, opID, nil)
	require.NoError(t, err)
	for _, job := range jobs.Jobs {
		stderr, err := env.YT.GetJobStderr(ctx, opID, job.ID, nil)
		require.NoError(t, err)
		require.Equal(t, []byte("hello\n"), stderr)
	}
}

func TestListOperations(t *testing.T) {
	t.Parallel()

	env, cancel := yttest.NewEnv(t)
	defer cancel()

	ctx, cancel := context.WithTimeout(context.Background(), time.Minute)
	defer cancel()

	inTable := tmpPath()
	outTable := tmpPath()

	for _, p := range []ypath.Path{inTable, outTable} {
		_, err := env.YT.CreateNode(ctx, p, yt.NodeTable, nil)
		require.NoError(t, err)
	}

	w, err := env.YT.WriteTable(ctx, inTable, nil)
	require.NoError(t, err)
	require.NoError(t, w.Write(map[string]interface{}{"a": int64(1)}))
	require.NoError(t, w.Commit())

	spec := map[string]interface{}{
		"input_table_paths":  []ypath.Path{inTable},
		"output_table_paths": []ypath.Path{outTable},
		"mapper": map[string]interface{}{
			"input_format":  "yson",
			"output_format": "yson",
			"command":       "echo hello >> /dev/stderr",
		},
	}

	opID, err := env.YT.StartOperation(ctx, yt.OperationMap, spec, nil)
	require.NoError(t, err)
	for {
		time.Sleep(time.Second)
		status, err := env.YT.GetOperation(ctx, opID, nil)
		require.NoError(t, err)
		if status.State == yt.StateCompleted {
			break
		}
	}
	ops, err := env.YT.ListOperations(ctx, nil)
	require.NoError(t, err)
	found := false
	for _, op := range ops.Operations {
		if op.ID == opID {
			found = true
		}
	}
	require.True(t, found, "Operation list must contain operation ID: %v", opID.String())
}
